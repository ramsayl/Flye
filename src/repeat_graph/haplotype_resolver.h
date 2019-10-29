//(c) 2016-2019 by Authors
//This file is a part of the Flye program.
//Released under the BSD license (see LICENSE file)

#pragma once

#include "repeat_graph.h"
#include "read_aligner.h"




class HaplotypeResolver
{
public:
	HaplotypeResolver(RepeatGraph& graph, ReadAligner& aligner,
					  const SequenceContainer& asmSeqs, 
					  const SequenceContainer& readSeqs):
		_graph(graph), _aligner(aligner), _asmSeqs(asmSeqs), 
		_readSeqs(readSeqs) {}

	int collapseHeterozygousLoops(bool removeAlternatives);
	int collapseHeterozygousBulges(bool removeAlternatives);
	int findComplexHaplotypes();

private:
	struct PathWithScore
	{
		GraphAlignment path;
		int score;
	};

	struct VariantPaths
	{
		VariantPaths(): startEdge(nullptr), endEdge(nullptr) {}
		GraphEdge* startEdge;
		GraphEdge* endEdge;
		std::vector<PathWithScore> altPaths;
	};

	VariantPaths findVariantSegment(GraphEdge* startEdge, 
									const std::vector<GraphAlignment>& alnignments,
									const std::unordered_set<GraphEdge*>& loopedEdges);

	RepeatGraph& _graph;
	ReadAligner& _aligner;
	const SequenceContainer& _asmSeqs;
	const SequenceContainer& _readSeqs;
};
