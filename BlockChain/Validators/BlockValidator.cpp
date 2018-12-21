#include "BlockValidator.h"
#include "TransactionBodyValidator.h"
#include "../Processors/BlockHeaderProcessor.h"
#include "../CommitmentUtil.h"

#include <Crypto.h>
#include <Consensus/Common.h>
#include <TxHashSet.h>

BlockValidator::BlockValidator(ITxHashSet* pTxHashSet)
	: m_pTxHashSet(pTxHashSet)
{

}

// Validates all the elements in a block that can be checked without additional data. 
// Includes commitment sums and kernels, Merkle trees, reward, etc.
bool BlockValidator::IsBlockValid(const FullBlock& block, const BlindingFactor& previousKernelOffset) const
{
	if (!TransactionBodyValidator().ValidateTransactionBody(block.GetTransactionBody(), true))
	{
		return false;
	}

	if (!VerifyKernelLockHeights(block))
	{
		return false;
	}
		
	if (!VerifyCoinbase(block))
	{
		return false;
	}

	BlindingFactor blockKernelOffset(CBigInteger<32>::ValueOf(0));

	// take the kernel offset for this block (block offset minus previous) and verify.body.outputs and kernel sums
	if (block.GetBlockHeader().GetTotalKernelOffset() == previousKernelOffset)
	{
		blockKernelOffset = CommitmentUtil::AddKernelOffsets(std::vector<BlindingFactor>({ block.GetBlockHeader().GetTotalKernelOffset() }), std::vector<BlindingFactor>({ previousKernelOffset }));
	}

	const bool kernelSumsValid = CommitmentUtil::VerifyKernelSums(block, 0 - Consensus::REWARD, blockKernelOffset);
	if (!kernelSumsValid)
	{
		return false;
	}

	// TODO: Validate MMRs

	return true;
}

// check we have no kernels with lock_heights greater than current height
// no tx can be included in a block earlier than its lock_height
bool BlockValidator::VerifyKernelLockHeights(const FullBlock& block) const
{
	const uint64_t height = block.GetBlockHeader().GetHeight();
	for (const TransactionKernel& kernel : block.GetTransactionBody().GetKernels())
	{
		if (kernel.GetLockHeight() > height)
		{
			return false;
		}
	}

	return true;
}

// Validate the coinbase outputs generated by miners.
// Check the sum of coinbase-marked outputs match the sum of coinbase-marked kernels accounting for fees.
bool BlockValidator::VerifyCoinbase(const FullBlock& block) const
{
	std::vector<Commitment> coinbaseCommitments;
	for (const TransactionOutput& output : block.GetTransactionBody().GetOutputs())
	{
		if ((output.GetFeatures() & EOutputFeatures::COINBASE_OUTPUT) == EOutputFeatures::COINBASE_OUTPUT)
		{
			coinbaseCommitments.push_back(output.GetCommitment());
		}
	}

	std::vector<Commitment> coinbaseKernelExcesses;
	for (const TransactionKernel& kernel : block.GetTransactionBody().GetKernels())
	{
		if ((kernel.GetFeatures() & EKernelFeatures::COINBASE_KERNEL) == EKernelFeatures::COINBASE_KERNEL)
		{
			coinbaseKernelExcesses.push_back(kernel.GetExcessCommitment());
		}
	}

	uint64_t reward = Consensus::REWARD;
	for (const TransactionKernel& kernel : block.GetTransactionBody().GetKernels())
	{
		reward += kernel.GetFee();
	}

	std::unique_ptr<Commitment> pRewardCommitment = Crypto::CommitTransparent(reward);
	if (pRewardCommitment == nullptr)
	{
		return false;
	}

	const std::vector<Commitment> overCommitment({ *pRewardCommitment });
	const std::unique_ptr<Commitment> pOutputAdjustedSum = Crypto::AddCommitments(coinbaseCommitments, overCommitment);

	const std::unique_ptr<Commitment> pKernelSum = Crypto::AddCommitments(coinbaseKernelExcesses, std::vector<Commitment>());

	// Verify the kernel sum equals the output sum accounting for block fees.
	if (pOutputAdjustedSum == nullptr || pKernelSum == nullptr)
	{
		return false;
	}

	return *pKernelSum == *pOutputAdjustedSum;
}