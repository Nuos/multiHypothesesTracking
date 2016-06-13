#ifndef JSON_MODEL_H
#define JSON_MODEL_H

#include <memory>
#include <vector>
#include <map>
#include <tuple>

#include "model.h"

namespace mht
{

/**
 * @brief Model specialized for Json loading and writing
 * @detail WARNING: at the moment you can only run either learn or infer once on the model. 
 *         Build a new one if you need it multiple times
 */
class JsonModel : public Model
{
public: 
    /**
     * @brief Read a model consisting of segmentation hypotheses and linking hypotheses from a json file
     * @param filename
     */
    void readFromJson(const std::string& filename);

    /**
     * @brief Export a found solution vector as a readable json file
     * 
     * @param filename where to save the result
     * @param sol the labeling to save
     */
    void saveResultToJson(const std::string& filename, const helpers::Solution& sol) const;

    /**
     * @brief Read in a ground truth solution (a boolean value per link) from a json file
     * 
     * @param filename where to find the ground truth
     * @return the solution as a vector of per-opengm-variable labelings
     */
    void setJsonGtFile(const std::string& filename);

    /**
     * @brief get the ground truth for learning from a JSON file
     * @return the solution vector that fits the initialized OpenGM model
     */
    virtual helpers::Solution getGroundTruth();

private:
    /**
     * @brief read linking hypothesis from Json and adds it to linkingHypotheses_
     * @details expects the json value to contain attributes "src"(helpers::IdLabelType), 
     *  "dest"(helpers::IdLabelType), and "features"(list of double)
     * 
     * @param entry json object for this hypothesis
     */
    void readLinkingHypothesis(const Json::Value& entry);

    /**
     * @brief read segmentation hypothesis from Json and adds it to segmentationHypotheses_
     * @details expects the json value to contain attributes "id"(helpers::IdLabelType) and "features"(list of double),
     *          as well as "divisionFeatures", "appearanceFeatures" and "disappearanceFeatures", where
     *          the presence of the latter two toggles the presence of an appearance or disappearance node.
     *          Hypotheses which do not have these, are not allowed to appear/disappear!
     * 
     * @param entry json object for this hypothesis
     */
    void readSegmentationHypothesis(const Json::Value& entry);

    /**
     * @brief read division hypothesis from Json
     *
     * @param entry json object for this hypothesis
     */
    void readDivisionHypotheses(const Json::Value& entry);

    /**
     * @brief read exclusion constraint from Json
     * @details expects the json array to be a list of ints representing ids
     * 
     * @param entry json object for this hypothesis
     */
    void readExclusionConstraints(const Json::Value& entry);

private:
    // ground truth filename
    std::string groundTruthFilename_;
};

} // end namespace mht

#endif // JSON_MODEL_H
