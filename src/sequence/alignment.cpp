//(c) 2016-2020 by Authors
//This file is a part of Flye program.
//Released under the BSD license (see LICENSE file)

#include <chrono>

#include "alignment.h"

#define HAVE_KALLOC
#include "kalloc.h"
#include "ksw2.h"
#undef HAVE_KALLOC

#include "edlib.h"

using namespace std::chrono;

namespace
{
	struct ThreadMemPool
	{
		ThreadMemPool():
			prevCleanup(system_clock::now() + seconds(rand() % 60))
		{
		   memPool = km_init();
		}
		~ThreadMemPool()
		{
		   km_destroy(memPool);
		}
		void cleanIter()
		{
			if ((system_clock::now() - prevCleanup) > seconds(60))
			{
				km_destroy(memPool);
				memPool = km_init();
				prevCleanup = system_clock::now();
			}
		}

		time_point<system_clock> prevCleanup;
		void* memPool;
	};
}

float getAlignmentCigarKsw(const DnaSequence& trgSeq, size_t trgBegin, size_t trgLen,
			   			   const DnaSequence& qrySeq, size_t qryBegin, size_t qryLen,
			   			   int matchScore, int misScore, int gapOpen, int gapExtend,
			   			   float maxAlnErr, std::vector<CigOp>& cigarOut)
{
	//static const int32_t MAX_JUMP = Config::get("maximum_jump");
	const int KMER_SIZE = Parameters::get().kmerSize;

	thread_local ThreadMemPool buf;
	thread_local std::vector<uint8_t> trgByte;
	thread_local std::vector<uint8_t> qryByte;
	buf.cleanIter();
	trgByte.assign(trgLen, 0);
	qryByte.assign(qryLen, 0);

	for (size_t i = 0; i < trgLen; ++i)
	{
		trgByte[i] = trgSeq.atRaw(i + trgBegin);
	}
	for (size_t i = 0; i < qryLen; ++i)
	{
		qryByte[i] = qrySeq.atRaw(i + qryBegin);
	}

	//int seqDiff = abs((int)trgByte.size() - (int)qryByte.size());
	//int bandWidth = seqDiff + MAX_JUMP;
	int bandWidth = std::max(10.0f, maxAlnErr * std::max(trgLen, qryLen));

	//substitution matrix
	int8_t a = matchScore;
	int8_t b = misScore < 0 ? misScore : -misScore; // a > 0 and b < 0
	int8_t subsMat[] = {a, b, b, b, 0, 
						b, a, b, b, 0, 
						b, b, a, b, 0, 
						b, b, b, a, 0, 
						0, 0, 0, 0, 0};

	ksw_extz_t ez;
	memset(&ez, 0, sizeof(ksw_extz_t));
	const int NUM_NUCL = 5;
	const int Z_DROP = -1;
	const int FLAG = KSW_EZ_APPROX_MAX | KSW_EZ_APPROX_DROP;
	const int END_BONUS = 0;
	//ksw_extf2_sse(0, qseq.size(), &qseq[0], tseq.size(), &tseq[0], matchScore,
	//		 	  misScore, gapOpen, bandWidth, Z_DROP, &ez);
	ksw_extz2_sse(buf.memPool, qryByte.size(), &qryByte[0], 
				  trgByte.size(), &trgByte[0], NUM_NUCL,
				  subsMat, gapOpen, gapExtend, bandWidth, Z_DROP, 
				  END_BONUS, FLAG, &ez);
	
	int numMatches = 0;
	int numMiss = 0;
	int numIndels = 0;

	cigarOut.clear();
	cigarOut.reserve((size_t)ez.n_cigar);

	//decode cigar
	size_t posQry = 0;
	size_t posTrg = 0;
	for (size_t i = 0; i < (size_t)ez.n_cigar; ++i)
	{
		int size = ez.cigar[i] >> 4;
		char op = "MID"[ez.cigar[i] & 0xf];
		//alnLength += size;

		if (op == 'M')
		{
			for (size_t i = 0; i < (size_t)size; ++i)
			{
				char match = "X="[size_t(trgByte[posTrg + i] == 
										 qryByte[posQry + i])];
				if (i == 0 || (match != cigarOut.back().op))
				{
					cigarOut.push_back({match, 1});
				}
				else
				{
					++cigarOut.back().len;
				}
				numMatches += int(match == '=');
				numMiss += int(match == 'X');
			}
			posQry += size;
			posTrg += size;
		}
		else if (op == 'I')
		{
			cigarOut.push_back({'I', size});
			posQry += size;
			numIndels += std::min(size, KMER_SIZE);
		}
		else //D
		{
			cigarOut.push_back({'D', size});
			posTrg += size;
			numIndels += std::min(size, KMER_SIZE);
		}
	}
	float errRate = 1 - float(numMatches) / (numMatches + numMiss + numIndels);

	kfree(buf.memPool, ez.cigar);
	return errRate;
}

float getAlignmentErrEdlib(const OverlapRange& ovlp,
					  	   const DnaSequence& trgSeq,
					  	   const DnaSequence& qrySeq,
						   float maxAlnErr)
{
	thread_local ThreadMemPool buf;
	thread_local std::vector<char> trgByte;
	thread_local std::vector<char> qryByte;
	buf.cleanIter();
	trgByte.assign(ovlp.curRange(), 0);
	qryByte.assign(ovlp.extRange(), 0);

	for (size_t i = 0; i < (size_t)ovlp.curRange(); ++i)
	{
		trgByte[i] = trgSeq.atRaw(i + ovlp.curBegin);
	}
	for (size_t i = 0; i < (size_t)ovlp.extRange(); ++i)
	{
		qryByte[i] = qrySeq.atRaw(i + ovlp.extBegin);
	}

	//int bandWidth = std::max(10.0f, maxAlnErr * std::max(ovlp.curRange(), 
	//													 ovlp.extRange()));
	//letting edlib find k byt iterating over powers of 2. Seems like
	//it is in fact a little faster, than having a hard upper limit.
	auto edlibCfg = edlibNewAlignConfig(-1, EDLIB_MODE_NW, 
										EDLIB_TASK_DISTANCE, nullptr, 0);
	auto result = edlibAlign(&qryByte[0], qryByte.size(),
							 &trgByte[0], trgByte.size(), edlibCfg);
	//Logger::get().debug() << result.editDistance << " " << result.alignmentLength;
	if (result.editDistance < 0)
	{
		return 1.0f;
	}
	return (float)result.editDistance / std::max(ovlp.curRange(), ovlp.extRange());
	//return (float)result.editDistance / result.alignmentLength;
}


float getAlignmentErrKsw(const OverlapRange& ovlp,
					  	 const DnaSequence& trgSeq,
					  	 const DnaSequence& qrySeq,
					  	 float maxAlnErr)
{
	std::vector<CigOp> decodedCigar;
	float errRate = getAlignmentCigarKsw(trgSeq, ovlp.curBegin, ovlp.curRange(),
							 			 qrySeq, ovlp.extBegin, ovlp.extRange(),
							 			 /*match*/ 1, /*mm*/ -2, /*gap open*/ 2, 
							 			 /*gap ext*/ 1, maxAlnErr, decodedCigar);

	//visualize alignents if needed
	/*if (showAlignment)
	{
		const int WIDTH = 100;
		for (size_t chunk = 0; chunk <= alnQry.size() / WIDTH; ++chunk)
		{
			for (size_t i = chunk * WIDTH; 
				 i < std::min((chunk + 1) * WIDTH, alnQry.size()); ++i)
			{
				std::cout << alnQry[i];
			}
			std::cout << "\n";
			for (size_t i = chunk * WIDTH; 
				 i < std::min((chunk + 1) * WIDTH, alnQry.size()); ++i)
			{
				std::cout << alnTrg[i];
			}
			std::cout << "\n\n";
		}
	}*/

	return errRate;
}


void decodeCigar(const std::vector<CigOp>& cigar, 
				 const DnaSequence& trgSeq, size_t trgBegin,
				 const DnaSequence& qrySeq, size_t qryBegin,
				 std::string& outAlnTrg, std::string& outAlnQry)
{
	/*std::vector<uint8_t> trgByte;
	std::vector<uint8_t> qryByte;
	trgByte.assign(ovlp.curRange(), 0);
	qryByte.assign(ovlp.extRange(), 0);
	for (size_t i = 0; i < (size_t)ovlp.curRange(); ++i)
	{
		trgByte[i] = trgSeq.atRaw(i + ovlp.curBegin);
	}
	for (size_t i = 0; i < (size_t)ovlp.extRange(); ++i)
	{
		qryByte[i] = qrySeq.atRaw(i + ovlp.extBegin);
	}*/

	//std::string strQ;
	//std::string strT;
	//std::string alnQry;
	//std::string alnTrg;

	//for (auto x : qryByte) strQ += "ACGT"[x];
	//for (auto x : trgByte) strT += "ACGT"[x];

	outAlnTrg.clear();
	outAlnQry.clear();
	size_t posQry = 0;
	size_t posTrg = 0;
	for (auto& op : cigar)
	{
		if (op.op == '=' || op.op == 'X')
		{
			//alnQry += strQ.substr(posQry, op.len);
			//alnTrg += strT.substr(posTrg, op.len);
			outAlnQry += qrySeq.substr(qryBegin + posQry, op.len).str();
			outAlnTrg += trgSeq.substr(trgBegin + posTrg, op.len).str();
			posQry += op.len;
			posTrg += op.len;
		}
		else if (op.op == 'I')
		{
			outAlnQry += qrySeq.substr(qryBegin + posQry, op.len).str();
			outAlnTrg += std::string(op.len, '-');
			posQry += op.len;
		}
		else
		{
			outAlnQry += std::string(op.len, '-');
			outAlnTrg += trgSeq.substr(trgBegin + posTrg, op.len).str();
			posTrg += op.len;
		}
	}
}
