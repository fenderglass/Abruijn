//(c) 2016 by Authors
//This file is a part of ABruijn program.
//Released under the BSD license (see LICENSE file)

#include <limits>
#include <cassert>
#include <algorithm>
#include <queue>
#include <iomanip>

#include "utility.h"
#include "extender.h"

ContigPath Extender::extendRead(FastaRecord::Id startRead)
{
	ContigPath contigPath;
	FastaRecord::Id curRead = startRead;
	contigPath.reads.push_back(curRead);
	_visitedReads.insert(curRead);
	_visitedReads.insert(curRead.rc());
	bool rightExtension = true;

	DEBUG_PRINT("Start Read: " << 
				_seqContainer.getIndex().at(startRead).description);

	std::unordered_set<FastaRecord::Id> curPathVisited;
	while(true)
	{
		FastaRecord::Id extRead = this->stepRight(curRead, startRead);

		if (extRead == startRead)	//circular
		{
			DEBUG_PRINT("Circular contig");
			contigPath.circular = true;
			break;
		}

		if (extRead != FastaRecord::ID_NONE) 
		{
			DEBUG_PRINT("Extension: " << 
				    	_seqContainer.getIndex().at(extRead).description);
			if (curPathVisited.count(extRead)) DEBUG_PRINT("Visited in this path");
			if (_visitedReads.count(extRead)) DEBUG_PRINT("Visited globally");
		}
		else
		{
			DEBUG_PRINT("No extension found");
		}

		if (_visitedReads.count(extRead) || extRead == FastaRecord::ID_NONE)
		{
			if (rightExtension)
			{
				DEBUG_PRINT("Changing direction");
				rightExtension = false;
				extRead = startRead.rc();
				std::reverse(contigPath.reads.begin(), contigPath.reads.end());
				for (size_t i = 0; i < contigPath.reads.size(); ++i) 
				{
					contigPath.reads[i] = contigPath.reads[i].rc();
				}
				contigPath.reads.pop_back();
			}
			else
			{
				DEBUG_PRINT("Linear contig");
				break;
			}
		}

		_visitedReads.insert(extRead);
		_visitedReads.insert(extRead.rc());
		curPathVisited.insert(extRead);
		curPathVisited.insert(extRead.rc());

		curRead = extRead;
		contigPath.reads.push_back(curRead);
	}

	//marking visited reads
	std::unordered_set<FastaRecord::Id> leftSupported;
	std::unordered_set<FastaRecord::Id> rightSupported;
	for (auto& readId : contigPath.reads)
	{
		for (auto& ovlp : _ovlpDetector.getOverlapIndex().at(readId))
		{
			if (this->isProperRightExtension(ovlp))
			{
				leftSupported.insert(ovlp.extId);
				rightSupported.insert(ovlp.extId.rc());
			}
			if (this->isProperLeftExtension(ovlp))
			{
				rightSupported.insert(ovlp.extId);
				leftSupported.insert(ovlp.extId.rc());
			}
		}
	}
	for (auto readId : leftSupported)
	{
		if (rightSupported.count(readId))
		{
			_visitedReads.insert(readId);
			_visitedReads.insert(readId.rc());
		}
	}
	//

	DEBUG_PRINT("Made " << contigPath.reads.size() - 1 << " extensions");
	return contigPath;
}

void Extender::assembleContigs()
{
	const int MIN_CONTIG_SIZE = 20;	//TODO: a smarter filter
	LOG_PRINT("Extending reads");
	_visitedReads.clear();

	while (true)
	{
		//choose a read for extension
		FastaRecord::Id startRead = FastaRecord::ID_NONE;
		for (auto& indexPair : _seqContainer.getIndex())
		{	
			if (!_visitedReads.count(indexPair.first) &&
				!_chimDetector.isChimeric(indexPair.first) &&
				this->countRightExtensions(indexPair.first, false) > 0 &&
				!this->isBranching(indexPair.first))
			{
				startRead = indexPair.first;
				break;
			}
		}
		if (startRead == FastaRecord::ID_NONE) break;

		ContigPath path = this->extendRead(startRead);
		if (path.reads.size() > MIN_CONTIG_SIZE)
		{
			_contigPaths.push_back(std::move(path));
		}
	}
	LOG_PRINT("Assembled " << _contigPaths.size() << " contigs");
}

float Extender::branchIndex(FastaRecord::Id readId)
{
	auto& overlaps = _ovlpDetector.getOverlapIndex().at(readId);
	int totalNewExt = 0;;
	int curExtensions = 0;
	for (auto& ovlp : overlaps)
	{
		if (this->isProperRightExtension(ovlp))
		{
			++curExtensions;
			totalNewExt += this->countRightExtensions(ovlp.extId, true);
		}
	}
	if (curExtensions == 0 || totalNewExt == 0) return 0.0f;

	float avgNew = (float)totalNewExt / curExtensions;
	float ratio = curExtensions / avgNew;
	return ratio;
}

//makes one extension to the right
FastaRecord::Id Extender::stepRight(FastaRecord::Id readId, 
									FastaRecord::Id startReadId)
{
	auto& overlaps = _ovlpDetector.getOverlapIndex().at(readId);
	std::unordered_set<FastaRecord::Id> extensions;

	for (auto& ovlp : overlaps)
	{
		assert(ovlp.curId != ovlp.extId);
		if (this->isProperRightExtension(ovlp)) 
		{
			if (ovlp.extId == startReadId) return startReadId;
			if (this->countRightExtensions(ovlp.extId, false) > 0)
			{
				extensions.insert(ovlp.extId);
			}
		}
	}
	
	//rank extension candidates
	std::unordered_map<FastaRecord::Id, 
					   std::tuple<int, int, int>> supportIndex;

	DEBUG_PRINT("Branch index " << this->branchIndex(readId));
	for (auto& extCandidate : extensions)
	{
		int leftSupport = 0;
		int rightSupport = 0;
		int ovlpSize = 0;
		int ovlpShift = 0;
		for (auto& ovlp : _ovlpDetector.getOverlapIndex().at(extCandidate))
		{
			if (ovlp.extId == readId) ovlpSize = ovlp.curRange();
			if (ovlp.extId == readId) ovlpShift = -ovlp.rightShift;
			if (!extensions.count(ovlp.extId)) continue;

			if (this->isProperRightExtension(ovlp)) ++rightSupport;
			if (this->isProperLeftExtension(ovlp)) ++leftSupport;
		}
		int branchingScore = 1 - this->isBranching(extCandidate);
		int minSupport = std::min(leftSupport, rightSupport);
		//minSupport = std::min(minSupport, 2);
		if (!this->isBranching(readId))
		{
			supportIndex[extCandidate] = std::make_tuple(branchingScore, minSupport, 
													 	 ovlpSize);
		}
		else
		{
			supportIndex[extCandidate] = std::make_tuple(ovlpSize, 0, 0);
		}
		DEBUG_PRINT("\t" << _seqContainer.getIndex().at(extCandidate).description
					<< "\t" << leftSupport << "\t" << rightSupport << "\t"
					<< std::fixed << std::setprecision(2) 
					<< this->branchIndex(extCandidate) << "\t" << ovlpSize
					<< "\t" << ovlpShift);
	}

	auto bestSupport = std::make_tuple(0, 0, 0);
	auto bestExtension = FastaRecord::ID_NONE;
	for (auto& extCandidate : extensions)
	{
		if (extCandidate == startReadId) return startReadId;

		if (supportIndex[extCandidate] > bestSupport)
		{
			bestSupport = supportIndex[extCandidate];
			bestExtension = extCandidate;
		}
	}


	if (bestExtension != FastaRecord::ID_NONE)
	{
		if (_chimDetector.isChimeric(bestExtension))
			DEBUG_PRINT("Chimeric extension! ");
		if (this->isBranching(bestExtension))
			DEBUG_PRINT("Branching extension! ");
	}
	return bestExtension;
}

int Extender::countRightExtensions(FastaRecord::Id readId, 
								   bool countVisited)
{
	int count = 0;
	for (auto& ovlp : _ovlpDetector.getOverlapIndex().at(readId))
	{

		if (countVisited || !_visitedReads.count(ovlp.extId))
		{
			if (this->isProperRightExtension(ovlp)) ++count;
		}
	}
	return count;
}

bool Extender::isBranching(FastaRecord::Id readId)
{
	return this->branchIndex(readId) > 2.0f;
}

//Checks if read is extended to the right
bool Extender::isProperRightExtension(const OverlapRange& ovlp)
{
	return !_chimDetector.isChimeric(ovlp.extId) && 
		   ovlp.rightShift > 0;
}

//Checks if read is extended to the left
bool Extender::isProperLeftExtension(const OverlapRange& ovlp)
{
	return !_chimDetector.isChimeric(ovlp.extId) &&
		   ovlp.leftShift < 0;
}