/*
// Copyright (C) 2018-2020 Intel Corporation
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
*/

#include <algorithm>
#include <string>
#include <vector>

#include <opencv2/imgproc/imgproc.hpp>

#include "models/hpe_model_openpose.h"
#include "models/openpose_decoder.h"

#include <samples/common.hpp>
#include <ngraph/ngraph.hpp>
#include <samples/ocv_common.hpp>

using namespace InferenceEngine;

HPEOpenPose::HPEOpenPose(const std::string& modelFileName, bool useAutoResize) :
    ModelBase(modelFileName),
    useAutoResize(useAutoResize) {
}

void HPEOpenPose::prepareInputsOutputs(InferenceEngine::CNNNetwork& cnnNetwork) {
    // --------------------------- Configure input & output -------------------------------------------------
    // --------------------------- Prepare input blobs ------------------------------------------------------
    ICNNNetwork::InputShapes inputShapes = cnnNetwork.getInputShapes();
    if (inputShapes.size() != 1)
        throw std::runtime_error("Demo supports topologies only with 1 input");
    inputsNames.push_back(inputShapes.begin()->first);
    SizeVector& inSizeVector = inputShapes.begin()->second;
    if (inSizeVector.size() != 4 || inSizeVector[0] != 1 || inSizeVector[1] != 3)
        throw std::runtime_error("3-channel 4-dimensional model's input is expected");

    InputInfo& inputInfo = *cnnNetwork.getInputsInfo().begin()->second;
    inputInfo.setPrecision(Precision::U8);
    inputInfo.getInputData()->setLayout(Layout::NCHW);

    // --------------------------- Prepare output blobs -----------------------------------------------------
    const OutputsDataMap& outputInfo = cnnNetwork.getOutputsInfo();
    if (outputInfo.size() != 2)
        throw std::runtime_error("Demo supports topologies only with 2 outputs");

    for (const auto& outputLayer: outputInfo) {
        outputLayer.second->setPrecision(Precision::FP32);
        outputLayer.second->setLayout(Layout::NCHW);
        outputsNames.push_back(outputLayer.first);
    }

    auto outputIt = outputInfo.begin();
    const SizeVector& pafsOutputDims = (*outputIt++).second->getTensorDesc().getDims();
    if (pafsOutputDims.size() != 4 || pafsOutputDims[0] != 1 || pafsOutputDims[1] != 2 * (keypointsNumber + 1))
        throw std::runtime_error("1x" + std::to_string(2 * (keypointsNumber + 1)) + "xHFMxWFM dimension of model's output is expected");
    const SizeVector& heatmapsOutputDims = (*outputIt++).second->getTensorDesc().getDims();
    if (heatmapsOutputDims.size() != 4 || heatmapsOutputDims[0] != 1 || heatmapsOutputDims[1] != keypointsNumber + 1)
        throw std::runtime_error("1x" + std::to_string(keypointsNumber + 1) + "xHFMxWFM dimension of model's heatmap is expected");
    if (pafsOutputDims[2] != heatmapsOutputDims[2] || pafsOutputDims[3] != heatmapsOutputDims[3])
        throw std::runtime_error("output and heatmap are expected to have matching last two dimensions");
}

int HPEOpenPose::reshape(InferenceEngine::CNNNetwork& cnnNetwork, const InputData& inputData) {
    ICNNNetwork::InputShapes inputShapes = cnnNetwork.getInputShapes();
    SizeVector& imageInputDims = inputShapes.begin()->second;
    inputLayerSize = cv::Size(imageInputDims[3], imageInputDims[2]);
    auto& imageSize = inputData.asRef<ImageInputData>().inputImage.size();
    double scale = static_cast<double>(inputLayerSize.height) / static_cast<double>(imageSize.height);
    cv::Size scaledSize(static_cast<int>(cvRound(imageSize.width * scale)),
                        static_cast<int>(cvRound(imageSize.height * scale)));
    cv::Size scaledImageSize(std::max(scaledSize.width, inputLayerSize.height),
                             inputLayerSize.height);
    int minHeight = std::min(scaledImageSize.height, scaledSize.height);
    scaledImageSize.width = static_cast<int>(std::ceil(
                scaledImageSize.width / static_cast<float>(stride))) * stride;
    pad(0) = static_cast<int>(std::floor((scaledImageSize.height - minHeight) / 2.0));
    pad(1) = static_cast<int>(std::floor((scaledImageSize.width - scaledSize.width) / 2.0));
    pad(2) = scaledImageSize.height - minHeight - pad(0);
    pad(3) = scaledImageSize.width - scaledSize.width - pad(1);

    if (scaledImageSize.width != (inputLayerSize.width - pad(1) - pad(3))) {
        return scaledImageSize.width;
    }
    else { 
        return 0;
    }
}

std::shared_ptr<InternalModelData> HPEOpenPose::preprocess(const InputData& inputData, InferenceEngine::InferRequest::Ptr& request) {
    auto& image = inputData.asRef<ImageInputData>().inputImage;

    if (useAutoResize) {
        /* Just set input blob containing read image. Resize and layout conversionx will be done automatically */
        request->SetBlob(inputsNames[0], wrapMat2Blob(image));
    }
    else {
        /* Resize and copy data from the image to the input blob */
        Blob::Ptr frameBlob = request->GetBlob(inputsNames[0]);
        InferenceEngine::LockedMemory<void> blobMapped = InferenceEngine::as<InferenceEngine::MemoryBlob>(frameBlob)->wmap();
        uint8_t* blob_data = blobMapped.as<uint8_t*>();
        cv::Mat resizedImage;
        double scale = inputLayerSize.height / static_cast<double>(image.rows);
        cv::resize(image, resizedImage, cv::Size(), scale, scale, cv::INTER_CUBIC);
        cv::Mat paddedImage;
        cv::copyMakeBorder(resizedImage, paddedImage, pad(0), pad(2), pad(1), pad(3),
                       cv::BORDER_CONSTANT, meanPixel);
        std::vector<cv::Mat> planes(3);
        for (size_t pId = 0; pId < planes.size(); pId++) {
            planes[pId] = cv::Mat(inputLayerSize, CV_8UC1, blob_data + pId * inputLayerSize.area());
            }
        cv::split(paddedImage, planes);
    }
    return std::shared_ptr<InternalModelData>(new InternalImageModelData(image.cols, image.rows));
}

std::unique_ptr<ResultBase> HPEOpenPose::postprocess(InferenceResult & infResult) {
    HumanPoseResult* result = new HumanPoseResult;
    *static_cast<ResultBase*>(result) = static_cast<ResultBase&>(infResult);

    auto outputMapped = infResult.outputsData[outputsNames[0]];
    auto heatMapsMapped = infResult.outputsData[outputsNames[1]];

    const SizeVector& outputDims = outputMapped->getTensorDesc().getDims();
    const SizeVector& heatMapDims = heatMapsMapped->getTensorDesc().getDims();

    const float *predictions = outputMapped->rmap().as<float*>();
    const float *heats = heatMapsMapped->rmap().as<float*>();

    const auto& internalData = infResult.internalModelData->asRef<InternalImageModelData>();

    std::vector<cv::Mat> heatMaps(keypointsNumber);
    for (size_t i = 0; i < heatMaps.size(); i++) {
        heatMaps[i] = cv::Mat(heatMapDims[2], heatMapDims[3], CV_32FC1,
                              reinterpret_cast<void*>(
                                  const_cast<float*>(
                                      heats + i * heatMapDims[2] * heatMapDims[3])));
    }
    resizeFeatureMaps(heatMaps);

    std::vector<cv::Mat> pafs(outputDims[1]);
    for (size_t i = 0; i < pafs.size(); i++) {
        pafs[i] = cv::Mat(heatMapDims[2], heatMapDims[3], CV_32FC1,
                          reinterpret_cast<void*>(
                              const_cast<float*>(
                                  predictions + i * heatMapDims[2] * heatMapDims[3])));
    }
    resizeFeatureMaps(pafs);

    std::vector<HumanPose> poses = extractPoses(heatMaps, pafs);

    cv::Size fullFeatureMapSize = heatMaps[0].size() * stride / upsampleRatio;
    float scaleX = internalData.inputImgWidth /
            static_cast<float>(fullFeatureMapSize.width - pad(1) - pad(3));
    float scaleY = internalData.inputImgHeight /
            static_cast<float>(fullFeatureMapSize.height - pad(0) - pad(2));
    for (auto& pose : poses) {
        for (auto& keypoint : pose.keypoints) {
            if (keypoint != cv::Point2f(-1, -1)) {
                keypoint.x *= stride / upsampleRatio;
                keypoint.x -= pad(1);
                keypoint.x *= scaleX;

                keypoint.y *= stride / upsampleRatio;
                keypoint.y -= pad(0);
                keypoint.y *= scaleY;
            }
        }
    }
    for (size_t i = 0; i < poses.size(); ++i) {
        result->poses.push_back(poses[i]);
    }

    return std::unique_ptr<ResultBase>(result);
}

void HPEOpenPose::resizeFeatureMaps(std::vector<cv::Mat>& featureMaps) const {
    for (auto& featureMap : featureMaps) {
        cv::resize(featureMap, featureMap, cv::Size(),
                   upsampleRatio, upsampleRatio, cv::INTER_CUBIC);
    }
}

class FindPeaksBody: public cv::ParallelLoopBody {
public:
    FindPeaksBody(const std::vector<cv::Mat>& heatMaps, float minPeaksDistance,
                  std::vector<std::vector<Peak> >& peaksFromHeatMap)
        : heatMaps(heatMaps),
          minPeaksDistance(minPeaksDistance),
          peaksFromHeatMap(peaksFromHeatMap) {}

    void operator()(const cv::Range& range) const override {
        for (int i = range.start; i < range.end; i++) {
            findPeaks(heatMaps, minPeaksDistance, peaksFromHeatMap, i);
        }
    }

private:
    const std::vector<cv::Mat>& heatMaps;
    float minPeaksDistance;
    std::vector<std::vector<Peak> >& peaksFromHeatMap;
};

std::vector<HumanPose> HPEOpenPose::extractPoses(
        const std::vector<cv::Mat>& heatMaps,
        const std::vector<cv::Mat>& pafs) const {
    std::vector<std::vector<Peak>> peaksFromHeatMap(heatMaps.size());
    FindPeaksBody findPeaksBody(heatMaps, minPeaksDistance, peaksFromHeatMap);
    cv::parallel_for_(cv::Range(0, static_cast<int>(heatMaps.size())),
                      findPeaksBody);
    int peaksBefore = 0;
    for (size_t heatmapId = 1; heatmapId < heatMaps.size(); heatmapId++) {
        peaksBefore += static_cast<int>(peaksFromHeatMap[heatmapId - 1].size());
        for (auto& peak : peaksFromHeatMap[heatmapId]) {
            peak.id += peaksBefore;
        }
    }
    std::vector<HumanPose> poses = groupPeaksToPoses(
                peaksFromHeatMap, pafs, keypointsNumber, midPointsScoreThreshold,
                foundMidPointsRatioThreshold, minJointsNumber, minSubsetScore);
    return poses;
}