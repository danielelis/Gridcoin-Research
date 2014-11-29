// Copyright (c) 2012-2013 The PPCoin developers
// Copyright (c) 2014 The GridCoin developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <boost/assign/list_of.hpp>

#include "kernel.h"
#include "txdb.h"


std::string YesNo(bool bin);
double GetPoSKernelPS2();

std::string RoundToString(double d, int place);
bool IsCPIDValidv2(MiningCPID& mc);
using namespace std;
MiningCPID DeserializeBoincBlock(std::string block);
bool IsCPIDValid_Retired(std::string cpid, std::string ENCboincpubkey);
MiningCPID GetMiningCPID();
StructCPID GetStructCPID();

typedef std::map<int, unsigned int> MapModifierCheckpoints;
/*
        ( 0, 0x0e00670bu )
        ( 1600, 0x1b3404a2 )
        ( 7000, 0x4da1176e )
*/

// Hard checkpoints of stake modifiers to ensure they are deterministic
static std::map<int, unsigned int> mapStakeModifierCheckpoints =
    boost::assign::map_list_of
        ( 0, 0x0e00670bu )
    ;

// Hard checkpoints of stake modifiers to ensure they are deterministic (testNet)
static std::map<int, unsigned int> mapStakeModifierCheckpointsTestNet =
    boost::assign::map_list_of
        ( 0, 0x0e00670bu )
    ;

// Get time weight
int64_t GetWeight(int64_t nIntervalBeginning, int64_t nIntervalEnd)
{
    // Kernel hash weight starts from 0 at the min age
    // this change increases active coins participating the hash and helps
    // to secure the network when proof-of-stake difficulty is low

    return min(nIntervalEnd - nIntervalBeginning - nStakeMinAge, (int64_t)nStakeMaxAge);
}

// Get the last stake modifier and its generation time from a given block
static bool GetLastStakeModifier(const CBlockIndex* pindex, uint64_t& nStakeModifier, int64_t& nModifierTime)
{
    if (!pindex)
        return error("GetLastStakeModifier: null pindex");
    while (pindex && pindex->pprev && !pindex->GeneratedStakeModifier())
        pindex = pindex->pprev;
    if (!pindex->GeneratedStakeModifier())
        return error("GetLastStakeModifier: no generation at genesis block");
    nStakeModifier = pindex->nStakeModifier;
    nModifierTime = pindex->GetBlockTime();
    return true;
}

// Get selection interval section (in seconds)
static int64_t GetStakeModifierSelectionIntervalSection(int nSection)
{
    assert (nSection >= 0 && nSection < 64);
    return (nModifierInterval * 63 / (63 + ((63 - nSection) * (MODIFIER_INTERVAL_RATIO - 1))));
}

// Get stake modifier selection interval (in seconds)
static int64_t GetStakeModifierSelectionInterval()
{
    int64_t nSelectionInterval = 0;
    for (int nSection=0; nSection<64; nSection++)
        nSelectionInterval += GetStakeModifierSelectionIntervalSection(nSection);
    return nSelectionInterval;
}

// select a block from the candidate blocks in vSortedByTimestamp, excluding
// already selected blocks in vSelectedBlocks, and with timestamp up to
// nSelectionIntervalStop.
static bool SelectBlockFromCandidates(vector<pair<int64_t, uint256> >& vSortedByTimestamp, map<uint256, const CBlockIndex*>& mapSelectedBlocks,
    int64_t nSelectionIntervalStop, uint64_t nStakeModifierPrev, const CBlockIndex** pindexSelected)
{
    bool fSelected = false;
    uint256 hashBest = 0;
    *pindexSelected = (const CBlockIndex*) 0;
    BOOST_FOREACH(const PAIRTYPE(int64_t, uint256)& item, vSortedByTimestamp)
    {
        if (!mapBlockIndex.count(item.second))
            return error("SelectBlockFromCandidates: failed to find block index for candidate block %s", item.second.ToString().c_str());
        const CBlockIndex* pindex = mapBlockIndex[item.second];
        if (fSelected && pindex->GetBlockTime() > nSelectionIntervalStop)
            break;
        if (mapSelectedBlocks.count(pindex->GetBlockHash()) > 0)
            continue;
        // compute the selection hash by hashing its proof-hash and the
        // previous proof-of-stake modifier
        CDataStream ss(SER_GETHASH, 0);
        ss << pindex->hashProof << nStakeModifierPrev;
        uint256 hashSelection = Hash(ss.begin(), ss.end());
        // the selection hash is divided by 2**32 so that proof-of-stake block
        // is always favored over proof-of-work block. this is to preserve
        // the energy efficiency property
        if (pindex->IsProofOfStake())
            hashSelection >>= 32;
        if (fSelected && hashSelection < hashBest)
        {
            hashBest = hashSelection;
            *pindexSelected = (const CBlockIndex*) pindex;
        }
        else if (!fSelected)
        {
            fSelected = true;
            hashBest = hashSelection;
            *pindexSelected = (const CBlockIndex*) pindex;
        }
    }
    if (fDebug && GetBoolArg("-printstakemodifier"))
        printf("SelectBlockFromCandidates: selection hash=%s\n", hashBest.ToString().c_str());
    return fSelected;
}

// Stake Modifier (hash modifier of proof-of-stake):
// The purpose of stake modifier is to prevent a txout (coin) owner from
// computing future proof-of-stake generated by this txout at the time
// of transaction confirmation. To meet kernel protocol, the txout
// must hash with a future stake modifier to generate the proof.
// Stake modifier consists of bits each of which is contributed from a
// selected block of a given block group in the past.
// The selection of a block is based on a hash of the block's proof-hash and
// the previous stake modifier.
// Stake modifier is recomputed at a fixed time interval instead of every
// block. This is to make it difficult for an attacker to gain control of
// additional bits in the stake modifier, even after generating a chain of
// blocks.
bool ComputeNextStakeModifier(const CBlockIndex* pindexPrev, uint64_t& nStakeModifier, bool& fGeneratedStakeModifier)
{
    nStakeModifier = 0;
    fGeneratedStakeModifier = false;
    if (!pindexPrev)
    {
        fGeneratedStakeModifier = true;
        return true;  // genesis block's modifier is 0
    }
    // First find current stake modifier and its generation block time
    // if it's not old enough, return the same stake modifier
    int64_t nModifierTime = 0;
    if (!GetLastStakeModifier(pindexPrev, nStakeModifier, nModifierTime))
        return error("ComputeNextStakeModifier: unable to get last modifier");
    if (fDebug)
    {
        printf("ComputeNextStakeModifier: prev modifier=0x%016"PRIx64" time=%s\n", nStakeModifier, DateTimeStrFormat(nModifierTime).c_str());
    }
    if (nModifierTime / nModifierInterval >= pindexPrev->GetBlockTime() / nModifierInterval)
        return true;

    // Sort candidate blocks by timestamp
    vector<pair<int64_t, uint256> > vSortedByTimestamp;
    vSortedByTimestamp.reserve(64 * nModifierInterval / GetTargetSpacing(pindexPrev->nHeight));
    int64_t nSelectionInterval = GetStakeModifierSelectionInterval();
    int64_t nSelectionIntervalStart = (pindexPrev->GetBlockTime() / nModifierInterval) * nModifierInterval - nSelectionInterval;
    const CBlockIndex* pindex = pindexPrev;
    while (pindex && pindex->GetBlockTime() >= nSelectionIntervalStart)
    {
        vSortedByTimestamp.push_back(make_pair(pindex->GetBlockTime(), pindex->GetBlockHash()));
        pindex = pindex->pprev;
    }
    int nHeightFirstCandidate = pindex ? (pindex->nHeight + 1) : 0;
    reverse(vSortedByTimestamp.begin(), vSortedByTimestamp.end());
    sort(vSortedByTimestamp.begin(), vSortedByTimestamp.end());

    // Select 64 blocks from candidate blocks to generate stake modifier
    uint64_t nStakeModifierNew = 0;
    int64_t nSelectionIntervalStop = nSelectionIntervalStart;
    map<uint256, const CBlockIndex*> mapSelectedBlocks;
    for (int nRound=0; nRound<min(64, (int)vSortedByTimestamp.size()); nRound++)
    {
        // add an interval section to the current selection round
        nSelectionIntervalStop += GetStakeModifierSelectionIntervalSection(nRound);
        // select a block from the candidates of current round
        if (!SelectBlockFromCandidates(vSortedByTimestamp, mapSelectedBlocks, nSelectionIntervalStop, nStakeModifier, &pindex))
            return error("ComputeNextStakeModifier: unable to select block at round %d", nRound);
        // write the entropy bit of the selected block
        nStakeModifierNew |= (((uint64_t)pindex->GetStakeEntropyBit()) << nRound);
        // add the selected block from candidates to selected list
        mapSelectedBlocks.insert(make_pair(pindex->GetBlockHash(), pindex));
        if (fDebug && GetBoolArg("-printstakemodifier"))
            printf("ComputeNextStakeModifier: selected round %d stop=%s height=%d bit=%d\n", nRound, DateTimeStrFormat(nSelectionIntervalStop).c_str(), pindex->nHeight, pindex->GetStakeEntropyBit());
    }

    // Print selection map for visualization of the selected blocks
    if (fDebug && GetBoolArg("-printstakemodifier"))
    {
        string strSelectionMap = "";
        // '-' indicates proof-of-work blocks not selected
        strSelectionMap.insert(0, pindexPrev->nHeight - nHeightFirstCandidate + 1, '-');
        pindex = pindexPrev;
        while (pindex && pindex->nHeight >= nHeightFirstCandidate)
        {
            // '=' indicates proof-of-stake blocks not selected
            if (pindex->IsProofOfStake())
                strSelectionMap.replace(pindex->nHeight - nHeightFirstCandidate, 1, "=");
            pindex = pindex->pprev;
        }
        BOOST_FOREACH(const PAIRTYPE(uint256, const CBlockIndex*)& item, mapSelectedBlocks)
        {
            // 'S' indicates selected proof-of-stake blocks
            // 'W' indicates selected proof-of-work blocks
            strSelectionMap.replace(item.second->nHeight - nHeightFirstCandidate, 1, item.second->IsProofOfStake()? "S" : "W");
        }
        printf("ComputeNextStakeModifier: selection height [%d, %d] map %s\n", nHeightFirstCandidate, pindexPrev->nHeight, strSelectionMap.c_str());
    }
    if (fDebug)
    {
        printf("ComputeNextStakeModifier: new modifier=0x%016"PRIx64" time=%s\n", nStakeModifierNew, DateTimeStrFormat(pindexPrev->GetBlockTime()).c_str());
    }

    nStakeModifier = nStakeModifierNew;
    fGeneratedStakeModifier = true;
    return true;
}

// The stake modifier used to hash for a stake kernel is chosen as the stake
// modifier about a selection interval later than the coin generating the kernel
static bool GetKernelStakeModifier(uint256 hashBlockFrom, uint64_t& nStakeModifier, int& nStakeModifierHeight, int64_t& nStakeModifierTime, bool fPrintProofOfStake)
{
    nStakeModifier = 0;
    if (!mapBlockIndex.count(hashBlockFrom))
        return error("GetKernelStakeModifier() : block not indexed");
    const CBlockIndex* pindexFrom = mapBlockIndex[hashBlockFrom];
    nStakeModifierHeight = pindexFrom->nHeight;
    nStakeModifierTime = pindexFrom->GetBlockTime();
    int64_t nStakeModifierSelectionInterval = GetStakeModifierSelectionInterval();
    const CBlockIndex* pindex = pindexFrom;
    // loop to find the stake modifier later by a selection interval
    while (nStakeModifierTime < pindexFrom->GetBlockTime() + nStakeModifierSelectionInterval)
    {
        if (!pindex->pnext)
        {   // reached best block; may happen if node is behind on block chain
            if (fPrintProofOfStake || (pindex->GetBlockTime() + nStakeMinAge - nStakeModifierSelectionInterval > GetAdjustedTime()))
                return error("GetKernelStakeModifier() : reached best block %s at height %d from block %s",
                    pindex->GetBlockHash().ToString().c_str(), pindex->nHeight, hashBlockFrom.ToString().c_str());
            else
                return false;
        }
        pindex = pindex->pnext;
        if (pindex->GeneratedStakeModifier())
        {
            nStakeModifierHeight = pindex->nHeight;
            nStakeModifierTime = pindex->GetBlockTime();
        }
    }
    nStakeModifier = pindex->nStakeModifier;
    return true;
}

// ppcoin kernel protocol
// cfoinstake must meet hash target according to the protocol:
// kernel (input 0) must meet the formula
//     hash(nStakeModifier + txPrev.block.nTime + txPrev.offset + txPrev.nTime + txPrev.vout.n + nTime) < bnTarget * nCoinDayWeight
// this ensures that the chance of getting a coinstake is proportional to the
// amount of coin age one owns.
// The reason this hash is chosen is the following:
//   nStakeModifier: scrambles computation to make it very difficult to precompute
//                  future proof-of-stake at the time of the coin's confirmation
//   txPrev.block.nTime: prevent nodes from guessing a good timestamp to
//                       generate transaction for future advantage
//   txPrev.offset: offset of txPrev inside block, to reduce the chance of
//                  nodes generating coinstake at the same time
//   txPrev.nTime: reduce the chance of nodes generating coinstake at the same
//                 time
//   txPrev.vout.n: output number of txPrev, to reduce the chance of nodes
//                  generating coinstake at the same time
//   block/tx hash should not be used here as they can be generated in vast
//   quantities so as to generate blocks faster, degrading the system back into
//   a proof-of-work situation.
//
int NewbieCompliesWithFirstTimeStakeWeightRule(const CBlock& blockFrom, std::string hashBoinc)
{
	// If newbie is not boincing, return 0
	// If newbie is a veteran, return 0
	// if newbie is an Investor, return 1

	// If newbie is boincing and not in the chain, Uninitialized Newbie, return 2
	// If newbie solved between 1-5 blocks, return 3
	// If newbie has reached level1, return 4
	// If newbie has reached level2, return 5
	//11-14-2014

	try
	{
		if (hashBoinc.length() > 1)
		{
			MiningCPID boincblock = DeserializeBoincBlock(hashBoinc);
			if (boincblock.cpid == "" || boincblock.cpid.length() < 6) return 100;  //Block has no CPID
	   	    if (boincblock.cpid == "INVESTOR") return 1;
	
			//CPID <> INVESTOR:
			if (boincblock.cpid != "INVESTOR") 
			{
    			if (boincblock.projectname == "") 	return 101;
	    		if (boincblock.rac < 100) 			return 102;
				//11-28-2014 CPIDV2->PrevBlock
				//Heinous Problem is Here:
				
				if (!IsCPIDValidv2(boincblock)) return 103;
				//Block CPID:11-29-2014
				//if (IsCPIDValid_Retired(boincblock.cpid,boincblock.enccpid)) return 136;

				//If we already have a consensus on the node, the cpid does not qualify
				if (mvMagnitudes.size() > 0)
				{
					StructCPID UntrustedHost = GetStructCPID();
					UntrustedHost = mvMagnitudes[boincblock.cpid]; //Contains Consensus Magnitude
					if (UntrustedHost.initialized)
					{
				
												
						if (UntrustedHost.Accuracy > MAX_NEWBIE_BLOCKS_LEVEL2) 
						{	
							//Veteran
							return 0;
						}

						if (UntrustedHost.Accuracy >= 0 && UntrustedHost.Accuracy < 4) 
						{
							//Newbie
							return 3;
						}
						if (UntrustedHost.Accuracy >= 4 && UntrustedHost.Accuracy < MAX_NEWBIE_BLOCKS)
						{
							//Newbie Level 1
							return 4;
						}
						if (UntrustedHost.Accuracy >= MAX_NEWBIE_BLOCKS && UntrustedHost.Accuracy < MAX_NEWBIE_BLOCKS_LEVEL2)
						{
							//Newbie Level 2
							return 5;
						}


					}
				}
				//printf("Newbie complies with first time stakeweight rule.[BlockFound]\r\n");
				//Uninitialized Newbie
				return 2;
			}
		}
		return 0;
	}
    catch (std::exception &e) 
	{
	    printf("Error while assessing Newbie Rule 1.\r\n");
		return 0;
	}
    catch(...)
	{
		printf("Error while assessing Newbie Rule 1[1].\r\n");
		return 0;
	}
	return 0;
}

double GetMagnitudeByHashBoinc(std::string hashBoinc)
{
	if (hashBoinc.length() > 1)
		{
			MiningCPID boincblock = DeserializeBoincBlock(hashBoinc);
			if (boincblock.cpid == "" || boincblock.cpid.length() < 6) return 0;  //Block has no CPID
			if (boincblock.cpid == "INVESTOR")  return 0;
   			if (boincblock.projectname == "") 	return 0;
    		if (boincblock.rac < 100) 			return 0;
			printf("?a");
			//11-29-2014
			//try
			//{
			//Linux problem here (.cpidv2, or boincblock.lastblockhash):
			//11-29-2014
			if (!IsCPIDValidv2(boincblock)) return 0;
			//}
			//catch(...)
			//{
			//	printf("Error 11292014");
			//	return 0;
			//}
			//if (IsCPIDValid_Retired(boincblock.cpid,boincblock.enccpid)) return 0;
			return boincblock.Magnitude;
			//StructCPID UntrustedHost = mvMagnitudes[boincblock.cpid]; //Contains Consensus Magnitude
			//return UntrustedHost.Magnitude;
		}
		return 0;
}


static bool CheckStakeKernelHashV1(unsigned int nBits, const CBlock& blockFrom, unsigned int nTxPrevOffset, 
	const CTransaction& txPrev, const COutPoint& prevout, unsigned int nTimeTx, uint256& hashProofOfStake, 
	uint256& targetProofOfStake, bool fPrintProofOfStake, std::string hashBoinc, bool checking_local)
{

	//Note: When client is checking locally for a good kernelhash, block.vtx[0] is still null, and block.vtx[1].GetValueOut() is null (for mint) so hashBoinc is provided separately
		

    if (nTimeTx < txPrev.nTime)  
	{
		// Transaction timestamp violation
		printf(".TimestampViolation.");
        return error("CheckStakeKernelHash() : nTime violation");
	}

    unsigned int nTimeBlockFrom = blockFrom.GetBlockTime();
    if (nTimeBlockFrom + nStakeMinAge > nTimeTx) 
	{
		// Min age requirement
		printf(".MinimumAgeRequirementViolation.,");
        return error("CheckStakeKernelHash() : min age violation");
	}

    CBigNum bnTargetPerCoinDay;
    bnTargetPerCoinDay.SetCompact(nBits);
    int64_t nValueIn = txPrev.vout[prevout.n].nValue;

    uint256 hashBlockFrom = blockFrom.GetHash();
	// Deserialize boinc hash for detailed logging //
	MiningCPID boincblock = DeserializeBoincBlock(hashBoinc);
	std::string cpid = boincblock.cpid;
    
	double mag_accuracy = 0;
	if (mvMagnitudes.size() > 0)
	{
			StructCPID UntrustedHost = GetStructCPID();
			UntrustedHost = mvMagnitudes[boincblock.cpid]; //Contains Consensus Magnitude
			if (UntrustedHost.initialized)
			{
						mag_accuracy = UntrustedHost.Accuracy;
			}
	}
	
	

	//WEIGHT MODIFICATION SECTION 2: Newbie stake allowance (11-13-2014)
	//This is primarily to allow a newbie researcher to get started with a low balance.
	//CBigNum bnCoinDayWeight = CBigNum(nValueIn + (5*COIN) ) * GetWeight((int64_t)txPrev.nTime, (int64_t)nTimeTx) / COIN / (24 * 60 * 60);
	int64_t NewbieStakeWeightModifier = 0;
	//double mint = (blockFrom.vtx[1].GetValueOut())/COIN;
	//	double subsidy = (blockFrom.vtx[0].GetValueOut())/COIN;
	//	std::string sSubsidy = RoundToString(subsidy,4);

	int NC = NewbieCompliesWithFirstTimeStakeWeightRule(blockFrom,hashBoinc);
	msMiningErrors2 = RoundToString(NC,0);
	int oNC = 0;
	// If newbie is not boincing, return 0
	// If newbie is a veteran, return 0
	// if newbie is an Investor, return 1
	// If newbie is boincing and not in the chain, Uninitialized Newbie, return 2
	// If newbie solved between 1-5 blocks, return 3
	// If newbie has reached level1, return 4
	// If newbie has reached level2, return 5
	//CPID v2 11-28-2014

	if (NC >= 2 && NC <= 5)
	{
		    //11-12-2014 Dynamic Newbie Weight
			double newbie_magnitude = GetMagnitudeByHashBoinc(hashBoinc);

		    //uint64_t nNetworkWeight = GetPoSKernelPS2();
			if (NC == 2)
			{
				NewbieStakeWeightModifier = newbie_magnitude*2500*COIN;
				oNC = NC;
			}
			else if (NC == 3)
			{
				NewbieStakeWeightModifier = newbie_magnitude*1500*COIN;
				//printf("NewbieModifierL2:  Mag %f, Mod  %"PRId64"  \r\n ",  newbie_magnitude,NewbieStakeWeightModifier);
				oNC = NC;
		    }
			else if (NC == 4)
			{
				NewbieStakeWeightModifier = newbie_magnitude*1000*COIN;
				//printf("NewbieModifierL3: Mag %f, Mod  %"PRId64" \r\n ",  newbie_magnitude,NewbieStakeWeightModifier);
				oNC = NC;
			}
			else if (NC == 5)
			{
				NewbieStakeWeightModifier = newbie_magnitude*500*COIN;
				//printf("NewbieModifierL3: Mag %f, Mod  %"PRId64" \r\n ",  newbie_magnitude,NewbieStakeWeightModifier);
				oNC = NC;
			}
	}
	else
	{
			NewbieStakeWeightModifier = 0*COIN;
			
	}

	int64_t boinc_seconds = 0;
	//int64_t Weight(int64_t nIntervalBeginning, int64_t nIntervalEnd)
    // Kernel hash weight starts from 0 at the min age
    // this change increases active coins participating the hash and helps
    // to secure the network when proof-of-stake difficulty is low
	//return min(nIntervalEnd - nIntervalBeginning - nStakeMinAge, (int64_t)nStakeMaxAge);
	
	CBigNum bnCoinDayWeight = 0;
	if (checking_local)
	{
		bnCoinDayWeight = CBigNum(nValueIn + NewbieStakeWeightModifier) * GetWeight((int64_t)txPrev.nTime, (int64_t)nTimeTx) / COIN / (24*60*60) / 3;
	}
	else
	{
		bnCoinDayWeight = CBigNum(nValueIn + NewbieStakeWeightModifier) * GetWeight((int64_t)txPrev.nTime, (int64_t)nTimeTx) / COIN / (24*60*60);
	}
	
    targetProofOfStake = (bnCoinDayWeight * bnTargetPerCoinDay).getuint256();

    // Calculate hash
    CDataStream ss(SER_GETHASH, 0);
    uint64_t nStakeModifier = 0;
    int nStakeModifierHeight = 0;
    int64_t nStakeModifierTime = 0;

    if (!GetKernelStakeModifier(hashBlockFrom, nStakeModifier, nStakeModifierHeight, nStakeModifierTime, fPrintProofOfStake))
    {
		printf("`");
		return false;
	}
    ss << nStakeModifier;

    ss << nTimeBlockFrom << nTxPrevOffset << txPrev.nTime << prevout.n << nTimeTx;
    hashProofOfStake = Hash(ss.begin(), ss.end());
    if (fPrintProofOfStake)
    {
        printf("CheckStakeKernelHash() : using modifier 0x%016"PRIx64" at height=%d timestamp=%s for block from height=%d timestamp=%s\n",
            nStakeModifier, nStakeModifierHeight,
            DateTimeStrFormat(nStakeModifierTime).c_str(),
            mapBlockIndex[hashBlockFrom]->nHeight,
            DateTimeStrFormat(blockFrom.GetBlockTime()).c_str());
            printf("CheckStakeKernelHash() : check modifier=0x%016"PRIx64" nTimeBlockFrom=%u nTxPrevOffset=%u nTimeTxPrev=%u nPrevout=%u nTimeTx=%u hashProof=%s\n",
            nStakeModifier,
            nTimeBlockFrom, nTxPrevOffset, txPrev.nTime, prevout.n, nTimeTx,
            hashProofOfStake.ToString().c_str());
    }

    // Now check if proof-of-stake hash meets target protocol
    if (CBigNum(hashProofOfStake) > (bnCoinDayWeight * bnTargetPerCoinDay))
	{   
		if (oNC==0)
		{
			//Use this area to log the submitters cpid and mint amount:
			//printf("!#"
			if (!checking_local)
			{
				//Dont print all this crap in the log if we are running a local miner:
				printf("Block does not meet target: hashboinc %s\r\n",hashBoinc.c_str());
				printf("Researcher vitals: NC %u, cpid %s, project %s, accuracy %f \r\n",NC,cpid.c_str(),boincblock.projectname.c_str(),mag_accuracy);
			}
			return false;
		}
	}
    if (fDebug && !fPrintProofOfStake)
    {
        printf("CheckStakeKernelHash() : using modifier 0x%016"PRIx64" at height=%d timestamp=%s for block from height=%d timestamp=%s\n",
            nStakeModifier, nStakeModifierHeight,
            DateTimeStrFormat(nStakeModifierTime).c_str(),
            mapBlockIndex[hashBlockFrom]->nHeight,
            DateTimeStrFormat(blockFrom.GetBlockTime()).c_str());
        printf("CheckStakeKernelHash() : pass modifier=0x%016"PRIx64" nTimeBlockFrom=%u nTxPrevOffset=%u nTimeTxPrev=%u nPrevout=%u nTimeTx=%u hashProof=%s\n",
            nStakeModifier,
            nTimeBlockFrom, nTxPrevOffset, txPrev.nTime, prevout.n, nTimeTx,
            hashProofOfStake.ToString().c_str());
    }
    return true;
}

// GridCoinCoin kernel protocol (credit goes to Black-Coin)
// coinstake must meet hash target according to the protocol:
// kernel (input 0) must meet the formula
//     hash(nStakeModifier + txPrev.block.nTime + txPrev.nTime + txPrev.vout.hash + txPrev.vout.n + nTime) < bnTarget * nWeight
// this ensures that the chance of getting a coinstake is proportional to the
// amount of coin age one owns.
// The reason this hash is chosen is the following:
//   nStakeModifier: scrambles computation to make it very difficult to precompute
//                   future proof-of-stake
//   txPrev.block.nTime: prevent nodes from guessing a good timestamp to
//                       generate transaction for future advantage
//   txPrev.nTime: slightly scrambles computation
//   txPrev.vout.hash: hash of txPrev, to reduce the chance of nodes
//                     generating coinstake at the same time
//   txPrev.vout.n: output number of txPrev, to reduce the chance of nodes
//                  generating coinstake at the same time
//   nTime: current timestamp
//   block/tx hash should not be used here as they can be generated in vast
//   quantities so as to generate blocks faster, degrading the system back into
//   a proof-of-work situation.
//
static bool CheckStakeKernelHashV2(CBlockIndex* pindexPrev, unsigned int nBits, unsigned int nTimeBlockFrom, 
	const CTransaction& txPrev, const COutPoint& prevout, unsigned int nTimeTx, uint256& hashProofOfStake, 
	uint256& targetProofOfStake, bool fPrintProofOfStake, std::string hashBoinc, bool checking_local)
{
	printf("StakeV2");
    if (nTimeTx < txPrev.nTime)  // Transaction timestamp violation
        return error("CheckStakeKernelHash() : nTime violation");

    if (nTimeBlockFrom + nStakeMinAge > nTimeTx) // Min age requirement
        return error("CheckStakeKernelHash() : min age violation");

    // Base target
    CBigNum bnTarget;
    bnTarget.SetCompact(nBits);

    // Weighted target
    int64_t nValueIn = txPrev.vout[prevout.n].nValue;
    CBigNum bnWeight = CBigNum(nValueIn);
    bnTarget *= bnWeight;

    targetProofOfStake = bnTarget.getuint256();

    uint64_t nStakeModifier = pindexPrev->nStakeModifier;
    int nStakeModifierHeight = pindexPrev->nHeight;
    int64_t nStakeModifierTime = pindexPrev->nTime;

    // Calculate hash
    CDataStream ss(SER_GETHASH, 0);
    ss << nStakeModifier << nTimeBlockFrom << txPrev.nTime << prevout.hash << prevout.n << nTimeTx;
    hashProofOfStake = Hash(ss.begin(), ss.end());

    if (fPrintProofOfStake)
    {
        printf("CheckStakeKernelHash() : using modifier 0x%016"PRIx64" at height=%d timestamp=%s for block from timestamp=%s\n",
            nStakeModifier, nStakeModifierHeight,
            DateTimeStrFormat(nStakeModifierTime).c_str(),
            DateTimeStrFormat(nTimeBlockFrom).c_str());
        printf("CheckStakeKernelHash() : check modifier=0x%016"PRIx64" nTimeBlockFrom=%u nTimeTxPrev=%u nPrevout=%u nTimeTx=%u hashProof=%s\n",
            nStakeModifier,
            nTimeBlockFrom, txPrev.nTime, prevout.n, nTimeTx,
            hashProofOfStake.ToString().c_str());
    }

    // Now check if proof-of-stake hash meets target protocol
    if (CBigNum(hashProofOfStake) > bnTarget)
        return false;

    if (fDebug && !fPrintProofOfStake)
    {
        printf("CheckStakeKernelHash() : using modifier 0x%016"PRIx64" at height=%d timestamp=%s for block from timestamp=%s\n",
            nStakeModifier, nStakeModifierHeight,
            DateTimeStrFormat(nStakeModifierTime).c_str(),
            DateTimeStrFormat(nTimeBlockFrom).c_str());
        printf("CheckStakeKernelHash() : pass modifier=0x%016"PRIx64" nTimeBlockFrom=%u nTimeTxPrev=%u nPrevout=%u nTimeTx=%u hashProof=%s\n",
            nStakeModifier,
            nTimeBlockFrom, txPrev.nTime, prevout.n, nTimeTx,
            hashProofOfStake.ToString().c_str());
    }

    return true;
}

bool CheckStakeKernelHash(CBlockIndex* pindexPrev, unsigned int nBits, const CBlock& blockFrom, unsigned int nTxPrevOffset, const CTransaction& txPrev, const COutPoint& prevout, unsigned int nTimeTx, uint256& hashProofOfStake, 
	uint256& targetProofOfStake, std::string hashBoinc, bool fPrintProofOfStake, bool checking_local)
{
    //if (IsProtocolV2(pindexPrev->nHeight+1))
    //    return ashV2(pindexPrev, nBits, blockFrom.GetBlockTime(), txPrev, prevout, nTimeTx, hashProofOfStake, targetProofOfStake, fPrintProofOfStake, hashBoinc, checking_local);
    //else
    return CheckStakeKernelHashV1(nBits, blockFrom, nTxPrevOffset, txPrev, prevout, nTimeTx, hashProofOfStake, targetProofOfStake, fPrintProofOfStake, hashBoinc, checking_local);
}

// Check kernel hash target and coinstake signature
bool CheckProofOfStake(CBlockIndex* pindexPrev, const CTransaction& tx, unsigned int nBits, 
	uint256& hashProofOfStake, uint256& targetProofOfStake, std::string hashBoinc, bool checking_local)
{
    if (!tx.IsCoinStake())
        return error("CheckProofOfStake() : called on non-coinstake %s", tx.GetHash().ToString().c_str());

    // Kernel (input 0) must match the stake hash target per coin age (nBits)
    const CTxIn& txin = tx.vin[0];

    // First try finding the previous transaction in database
    CTxDB txdb("r");
    CTransaction txPrev;
    CTxIndex txindex;
    if (!txPrev.ReadFromDisk(txdb, txin.prevout, txindex))
        return tx.DoS(1, error("CheckProofOfStake() : INFO: read txPrev failed"));  // previous transaction not in main chain, may occur during initial download

    // Verify signature
    if (!VerifySignature(txPrev, tx, 0, 0))
        return tx.DoS(100, error("CheckProofOfStake() : VerifySignature failed on coinstake %s", tx.GetHash().ToString().c_str()));

    // Read block header
    CBlock block;
    if (!block.ReadFromDisk(txindex.pos.nFile, txindex.pos.nBlockPos, false))
        return fDebug? error("CheckProofOfStake() : read block failed") : false; // unable to read block of previous transaction


	if (false)
	{
		//Verify solver did not solve back to back blocks:
		MiningCPID NewStakeBlock = DeserializeBoincBlock(hashBoinc);
		int nGrandfather = 55000;
		if (NewStakeBlock.GRCAddress.length() > 3  && pindexPrev->nHeight > nGrandfather)
		{
			CBlock prior_block;
			if (!prior_block.ReadFromDisk(pindexPrev))    return error("CheckProofOfStake() : read Prior block failed!");
		
			//11-21-2014
			if (prior_block.vtx.size() > 0)
			{
				MiningCPID PriorStakeBlock = DeserializeBoincBlock(prior_block.vtx[0].hashBoinc);
				if (PriorStakeBlock.GRCAddress == NewStakeBlock.GRCAddress)
				{
					return error("CheckProofOfStake() : Back To Back GRC Address found on two blocks in a row.  Rejected!\r\n");
				}
			}
	
		}
	}

	printf("-");

    if (!CheckStakeKernelHash(pindexPrev, nBits, block, txindex.pos.nTxPos - txindex.pos.nBlockPos, txPrev, txin.prevout, tx.nTime, hashProofOfStake, 
		targetProofOfStake, hashBoinc, fDebug, checking_local))
	{
		uint256 diff1 = hashProofOfStake - targetProofOfStake;
		uint256 diff2 = targetProofOfStake - hashProofOfStake;
        return tx.DoS(1, error("CheckProofOfStake() : INFO: check kernel failed on coinstake %s, hashProof=%s, target=%s, offby1: %s, OffBy2: %s", tx.GetHash().ToString().c_str(), hashProofOfStake.ToString().c_str(), targetProofOfStake.ToString().c_str(), diff1.ToString().c_str(), diff2.ToString().c_str())); // may occur during initial download or if behind on block chain sync

	}

    return true;
}

// Check whether the coinstake timestamp meets protocol
bool CheckCoinStakeTimestamp(int nHeight, int64_t nTimeBlock, int64_t nTimeTx)
{
    if (IsProtocolV2(nHeight))
        return (nTimeBlock == nTimeTx) && ((nTimeTx & STAKE_TIMESTAMP_MASK) == 0);
    else
        return (nTimeBlock == nTimeTx);
}

// Get stake modifier checksum
unsigned int GetStakeModifierChecksum(const CBlockIndex* pindex)
{
    assert (pindex->pprev || pindex->GetBlockHash() == (!fTestNet ? hashGenesisBlock : hashGenesisBlockTestNet));
    // Hash previous checksum with flags, hashProofOfStake and nStakeModifier
    CDataStream ss(SER_GETHASH, 0);
    if (pindex->pprev)
        ss << pindex->pprev->nStakeModifierChecksum;
    ss << pindex->nFlags << (pindex->IsProofOfStake() ? pindex->hashProof : 0) << pindex->nStakeModifier;
    uint256 hashChecksum = Hash(ss.begin(), ss.end());
    hashChecksum >>= (256 - 32);
    return hashChecksum.Get64();
}

// Check stake modifier hard checkpoints
bool CheckStakeModifierCheckpoints(int nHeight, unsigned int nStakeModifierChecksum)
{
    MapModifierCheckpoints& checkpoints = (fTestNet ? mapStakeModifierCheckpointsTestNet : mapStakeModifierCheckpoints);

    if (checkpoints.count(nHeight))
        return nStakeModifierChecksum == checkpoints[nHeight];
    return true;
}
