#include "model.h"
#include <json/json.h>
#include <fstream>
#include <stdexcept>
#include <numeric>
#include <sstream>

#ifdef WITH_CPLEX
#include <opengm/inference/lpcplex2.hxx>
#else
#include <opengm/inference/lpgurobi2.hxx>
#endif

#include <opengm/learning/struct-max-margin.hxx>

namespace mht
{
	
void Model::readFromJson(const std::string& filename)
{
	std::ifstream input(filename.c_str());
	if(!input.good())
		throw std::runtime_error("Could not open JSON model file " + filename);

	Json::Value root;
	input >> root;

	const Json::Value segmentationHypotheses = root[JsonTypeNames[JsonTypes::Segmentations]];
	std::cout << "\tcontains " << segmentationHypotheses.size() << " segmentation hypotheses" << std::endl;
	
	for(int i = 0; i < (int)segmentationHypotheses.size(); i++)
	{
		const Json::Value jsonHyp = segmentationHypotheses[i];
		SegmentationHypothesis hyp;
		int id = hyp.readFromJson(jsonHyp);
		segmentationHypotheses_[id] = hyp;
	}

	const Json::Value linkingHypotheses = root[JsonTypeNames[JsonTypes::Links]];
	std::cout << "\tcontains " << linkingHypotheses.size() << " linking hypotheses" << std::endl;
	for(int i = 0; i < (int)linkingHypotheses.size(); i++)
	{
		const Json::Value jsonHyp = linkingHypotheses[i];
		std::shared_ptr<LinkingHypothesis> hyp = std::make_shared<LinkingHypothesis>();
		std::pair<int, int> ids = hyp->readFromJson(jsonHyp);
		hyp->registerWithSegmentations(segmentationHypotheses_);
		linkingHypotheses_[ids] = hyp;
	}

	const Json::Value exclusions = root[JsonTypeNames[JsonTypes::Exclusions]];
	std::cout << "\tcontains " << exclusions.size() << " exclusions" << std::endl;
	for(int i = 0; i < (int)exclusions.size(); i++)
	{
		const Json::Value jsonExc = exclusions[i];
		exclusionConstraints_.push_back(ExclusionConstraint());
		ExclusionConstraint& exclusion = exclusionConstraints_.back();
		exclusion.readFromJson(jsonExc);
	}
}

size_t Model::computeNumWeights()
{
	int numDetFeatures = -1;
	int numDivFeatures = -1;
	int numAppFeatures = -1;
	int numDisFeatures = -1;
	int numLinkFeatures = -1;

	auto checkNumFeatures = [](const SegmentationHypothesis::Variable& var, int& previousNumFeats, const std::string& name)
	{
		if(previousNumFeats < 0 && var.getNumFeatures() > 0)
			previousNumFeats = var.getNumFeatures();
		else
			if(var.getNumFeatures() > 0 && var.getNumFeatures() != previousNumFeats)
				throw std::runtime_error(name + " do not have the same number of features!");
	};

	for(auto iter = segmentationHypotheses_.begin(); iter != segmentationHypotheses_.end() ; ++iter)
	{
		checkNumFeatures(iter->second.getDetectionVariable(), numDetFeatures, "Detections");
		checkNumFeatures(iter->second.getDivisionVariable(), numDivFeatures, "Divisions");
		checkNumFeatures(iter->second.getAppearanceVariable(), numAppFeatures, "Appearances");
		checkNumFeatures(iter->second.getDisappearanceVariable(), numDisFeatures, "Disappearances");
	}

	for(auto iter = linkingHypotheses_.begin(); iter != linkingHypotheses_.end() ; ++iter)
	{
		if(numLinkFeatures < 0)
			numLinkFeatures = iter->second->getNumFeatures();
		else
			if(iter->second->getNumFeatures() != numLinkFeatures)
				throw std::runtime_error("Links do not have the same number of features!");
	}

	// we don't want -1 weights, 
	numDetFeatures_ = std::max((int)0, numDetFeatures);
	numDivFeatures_ = std::max((int)0, numDivFeatures);
	numAppFeatures_ = std::max((int)0, numAppFeatures);
	numDisFeatures_ = std::max((int)0, numDisFeatures);
	numLinkFeatures_ = std::max((int)0, numLinkFeatures);

	// we need two sets of weights for all features to represent state "on" and "off"!
	return 2 * (numDetFeatures_ + numDivFeatures_ + numLinkFeatures_ + numAppFeatures_ + numDisFeatures_);
}

void Model::initializeOpenGMModel(WeightsType& weights)
{
	// make sure the numbers of features are initialized
	computeNumWeights();

	std::cout << "Initializing opengm model..." << std::endl;
	// we need two sets of weights for all features to represent state "on" and "off"!
	size_t numLinkWeights = 2 * numLinkFeatures_;
	std::vector<size_t> linkWeightIds(numLinkWeights);
	std::iota(linkWeightIds.begin(), linkWeightIds.end(), 0); // fill with increasing values starting at 0

	// first add all link variables, because segmentations will use them when defining constraints
	for(auto iter = linkingHypotheses_.begin(); iter != linkingHypotheses_.end() ; ++iter)
	{
		iter->second->addToOpenGMModel(model_, weights, linkWeightIds);
	}

	size_t numDetWeights = 2 * numDetFeatures_;
	std::vector<size_t> detWeightIds(numDetWeights);
	std::iota(detWeightIds.begin(), detWeightIds.end(), numLinkWeights); // fill with increasing values starting at 0

	size_t numDivWeights = 2 * numDivFeatures_;
	std::vector<size_t> divWeightIds(numDivWeights);
	std::iota(divWeightIds.begin(), divWeightIds.end(), numLinkWeights + numDetWeights);

	size_t numAppWeights = 2 * numAppFeatures_;
	std::vector<size_t> appWeightIds(numAppWeights);
	std::iota(appWeightIds.begin(), appWeightIds.end(), numLinkWeights + numDetWeights + numDivWeights);

	size_t numDisWeights = 2 * numDisFeatures_;
	std::vector<size_t> disWeightIds(numDisWeights);
	std::iota(disWeightIds.begin(), disWeightIds.end(), numLinkWeights + numDetWeights + numDivWeights + numAppWeights);

	for(auto iter = segmentationHypotheses_.begin(); iter != segmentationHypotheses_.end() ; ++iter)
	{
		iter->second.addToOpenGMModel(model_, weights, detWeightIds, divWeightIds, appWeightIds, disWeightIds);
	}

	for(auto iter = exclusionConstraints_.begin(); iter != exclusionConstraints_.end() ; ++iter)
	{
		iter->addToOpenGMModel(model_, segmentationHypotheses_);
	}
}

Solution Model::infer(const std::vector<ValueType>& weights)
{
	// use weights that were given
	WeightsType weightObject(computeNumWeights());
	assert(weights.size() == weightObject.numberOfWeights());
	for(size_t i = 0; i < weights.size(); i++)
		weightObject.setWeight(i, weights[i]);
	initializeOpenGMModel(weightObject);

#ifdef WITH_CPLEX
	std::cout << "Using cplex optimizer" << std::endl;
	typedef opengm::LPCplex2<GraphicalModelType, opengm::Minimizer> OptimizerType;
#else
	std::cout << "Using gurobi optimizer" << std::endl;
	typedef opengm::LPGurobi2<GraphicalModelType, opengm::Minimizer> OptimizerType;
#endif
	OptimizerType::Parameter optimizerParam;
	optimizerParam.integerConstraintNodeVar_ = true;
	optimizerParam.relaxation_ = OptimizerType::Parameter::TightPolytope;
	optimizerParam.verbose_ = true;
	optimizerParam.useSoftConstraints_ = false;

	OptimizerType optimizer(model_, optimizerParam);

	Solution solution(model_.numberOfVariables());
	OptimizerType::VerboseVisitorType optimizerVisitor;
	optimizer.infer(optimizerVisitor);
	optimizer.arg(solution);
	std::cout << "solution has energy: " << optimizer.value() << std::endl;

	std::cout << " found solution: " << solution << std::endl;

	return solution;
}

std::vector<ValueType> Model::learn(const std::string& gt_filename)
{
	DatasetType dataset;
	WeightsType initialWeights(computeNumWeights());
	dataset.setWeights(initialWeights);
	initializeOpenGMModel(dataset.getWeights());

	Solution gt = readGTfromJson(gt_filename);
	dataset.pushBackInstance(model_, gt);
	
	std::cout << "Done setting up dataset, creating learner" << std::endl;
	opengm::learning::StructMaxMargin<DatasetType>::Parameter learnerParam;
	opengm::learning::StructMaxMargin<DatasetType> learner(dataset, learnerParam);

#ifdef WITH_CPLEX
	typedef opengm::LPCplex2<GraphicalModelType, opengm::Minimizer> OptimizerType;
#else
	typedef opengm::LPGurobi2<GraphicalModelType, opengm::Minimizer> OptimizerType;
#endif
	
	OptimizerType::Parameter optimizerParam;
	optimizerParam.integerConstraintNodeVar_ = true;
	optimizerParam.relaxation_ = OptimizerType::Parameter::TightPolytope;
	optimizerParam.verbose_ = true;
	optimizerParam.useSoftConstraints_ = false;

	std::cout << "Calling learn()..." << std::endl;
	learner.learn<OptimizerType>(optimizerParam); 
	std::cout << "extracting weights" << std::endl;
	const WeightsType& finalWeights = learner.getWeights();
	std::vector<double> resultWeights;
	for(size_t i = 0; i < finalWeights.numberOfWeights(); ++i)
		resultWeights.push_back(finalWeights.getWeight(i));
	return resultWeights;
}

Solution Model::readGTfromJson(const std::string& filename)
{
	std::ifstream input(filename.c_str());
	if(!input.good())
		throw std::runtime_error("Could not open JSON ground truth file " + filename);

	Json::Value root;
	input >> root;

	const Json::Value linkingResults = root[JsonTypeNames[JsonTypes::LinkResults]];
	std::cout << "\tcontains " << linkingResults.size() << " linking annotations" << std::endl;

	// create a solution vector that holds a value for each segmentation / detection / link
	Solution solution(model_.numberOfVariables(), 0);

	// first set all source nodes to active. If a node is already active, this means a division
	for(int i = 0; i < linkingResults.size(); ++i)
	{
		const Json::Value jsonHyp = linkingResults[i];
		int srcId = jsonHyp[JsonTypeNames[JsonTypes::SrcId]].asInt();
		int destId = jsonHyp[JsonTypeNames[JsonTypes::DestId]].asInt();
		bool value = jsonHyp[JsonTypeNames[JsonTypes::Value]].asBool();
		if(value)
		{
			// try to find link
			if(linkingHypotheses_.find(std::make_pair(srcId, destId)) == linkingHypotheses_.end())
			{
				std::stringstream s;
				s << "Cannot find link to annotate: " << srcId << " to " << destId;
				throw std::runtime_error(s.str());
			}
			
			// set link active
			std::shared_ptr<LinkingHypothesis> hyp = linkingHypotheses_[std::make_pair(srcId, destId)];
			solution[hyp->getOpenGMVariableId()] = 1;

			// set source active, if it was active already then this is a division
			if(solution[segmentationHypotheses_[srcId].getDetectionVariable().getOpenGMVariableId()] == 1)
				solution[segmentationHypotheses_[srcId].getDivisionVariable().getOpenGMVariableId()] = 1;
			else
			{
				if(solution[segmentationHypotheses_[srcId].getDetectionVariable().getOpenGMVariableId()] == 1)
					throw std::runtime_error("A source node has been used more than once!");
				solution[segmentationHypotheses_[srcId].getDetectionVariable().getOpenGMVariableId()] = 1;
			}
		}
	}

	// enable target nodes so that the last node of each track is also active
	for(int i = 0; i < linkingResults.size(); ++i)
	{
		const Json::Value jsonHyp = linkingResults[i];
		int srcId = jsonHyp[JsonTypeNames[JsonTypes::SrcId]].asInt();
		int destId = jsonHyp[JsonTypeNames[JsonTypes::DestId]].asInt();
		bool value = jsonHyp[JsonTypeNames[JsonTypes::Value]].asBool();

		if(value)
		{
			solution[segmentationHypotheses_[destId].getDetectionVariable().getOpenGMVariableId()] = 1;
		}
	}

	for(auto iter = segmentationHypotheses_.begin(); iter != segmentationHypotheses_.end() ; ++iter)
	{
		size_t detValue = solution[iter->second.getDetectionVariable().getOpenGMVariableId()];

		if(detValue > 0)
		{
			// each variable that has no active incoming links but is active should have its appearance variables set to 1
			if(iter->second.getNumActiveIncomingLinks(solution) == 0)
			{
				if(iter->second.getAppearanceVariable().getOpenGMVariableId() == -1)
				{
					std::stringstream s;
					s << "Segmentation Hypothesis: " << iter->first << " - GT contains appearing variable that has no appearance features set!";
					throw std::runtime_error(s.str());
				}
				else
				{
					solution[iter->second.getAppearanceVariable().getOpenGMVariableId()] = 1;
				}
			}

			// each variable that has no active outgoing links but is active should have its disappearance variables set to 1
			if(iter->second.getNumActiveOutgoingLinks(solution) == 0)
			{
				if(iter->second.getDisappearanceVariable().getOpenGMVariableId() == -1)
				{
					std::stringstream s;
					s << "Segmentation Hypothesis: " << iter->first << " - GT contains disappearing variable that has no disappearance features set!";
					throw std::runtime_error(s.str());
				}
				else
				{
					solution[iter->second.getDisappearanceVariable().getOpenGMVariableId()] = 1;
				}
			}
		}
	}

	std::cout << "found gt solution: " << solution << std::endl;

	return solution;
}

bool Model::verifySolution(const Solution& sol) const
{
	std::cout << "Checking solution..." << std::endl;

	bool valid = true;

	// check that all exclusions are obeyed
	for(auto iter = exclusionConstraints_.begin(); iter != exclusionConstraints_.end() ; ++iter)
	{
		if(!iter->verifySolution(sol, segmentationHypotheses_))
		{
			std::cout << "\tFound violated exclusion constraint " << std::endl;
			valid = false;
		}
	}

	// check that flow-conservation + division constraints are satisfied
	for(auto iter = segmentationHypotheses_.begin(); iter != segmentationHypotheses_.end() ; ++iter)
	{
		if(!iter->second.verifySolution(sol))
		{
			std::cout << "\tFound violated flow conservation constraint " << std::endl;
			valid = false;
		}
	}

	return valid;
}

void Model::saveResultToJson(const std::string& filename, const Solution& sol) const
{
	std::ofstream output(filename.c_str());
	if(!output.good())
		throw std::runtime_error("Could not open JSON result file for saving: " + filename);

	Json::Value root;
	Json::Value& linksJson = root[JsonTypeNames[JsonTypes::LinkResults]];

	for(auto iter = linkingHypotheses_.begin(); iter != linkingHypotheses_.end() ; ++iter)
	{
		bool value = false;
		if(sol[iter->second->getOpenGMVariableId()] > 0)
			value = true;
		linksJson.append(iter->second->toJson(value));
	}

	if(!linksJson.isArray())
		throw std::runtime_error("Cannot save results to non-array JSON entry");

	output << root << std::endl;
}

void Model::toDot(const std::string& filename, const Solution* sol) const
{
	std::ofstream out_file(filename.c_str());

    if(!out_file.good())
    {
        throw std::runtime_error("Could not open file " + filename + " to save graph to");
    }

    out_file << "digraph G {\n";

    // nodes
    for(auto iter = segmentationHypotheses_.begin(); iter != segmentationHypotheses_.end() ; ++iter)
		iter->second.toDot(out_file, sol);

	// links
	for(auto iter = linkingHypotheses_.begin(); iter != linkingHypotheses_.end() ; ++iter)
		iter->second->toDot(out_file, sol);

	// exclusions
	for(auto iter = exclusionConstraints_.begin(); iter != exclusionConstraints_.end() ; ++iter)
		iter->toDot(out_file);
	
    out_file << "}";
}

std::vector<std::string> Model::getWeightDescriptions()
{
	std::vector<std::string> descriptions;
	computeNumWeights();

	auto addVariableWeightDescriptions = [&](size_t numFeatures, const std::string& name)
	{
		// each variable has duplicate features for state 0 and state 1
		for(size_t state = 0; state < 2; ++state)
		{
			for(size_t f = 0; f < numFeatures; ++f)
			{
				// append this variable's state/feature combination description
				std::stringstream d;
				d << name << " = " << state << " - feature " << f;
				descriptions.push_back(d.str());
			}
		}
	};

	addVariableWeightDescriptions(numDetFeatures_, "Detection");
	addVariableWeightDescriptions(numDivFeatures_, "Division");
	addVariableWeightDescriptions(numAppFeatures_, "Appearance");
	addVariableWeightDescriptions(numDisFeatures_, "Disappearance");
	addVariableWeightDescriptions(numLinkFeatures_, "Link");

	return descriptions;
}

} // end namespace mht