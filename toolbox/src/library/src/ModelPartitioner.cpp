/*
 * Copyright (C) 2018 Istituto Italiano di Tecnologia (IIT)
 * All rights reserved.
 *
 * This software may be modified and distributed under the terms of the
 * GNU Lesser General Public License v2.1 or any later version.
 */

#include "ModelPartitioner.h"
#include "Base/Configuration.h"
#include "Base/RobotInterface.h"
#include "Core/BlockInformation.h"
#include "Core/Log.h"
#include "Core/Parameter.h"
#include "Core/Parameters.h"
#include "Core/Signal.h"

#include <ostream>
#include <unordered_map>
#include <vector>

using namespace wbt;
const std::string ModelPartitioner::ClassName = "ModelPartitioner";

// INDICES: PARAMETERS, INPUTS, OUTPUT
// ===================================

enum ParamIndex
{
    Bias = WBBlock::NumberOfParameters - 1,
    Direction
};

// BLOCK PIMPL
// ===========

class ModelPartitioner::impl
{
public:
    bool vectorToControlBoards;
    std::shared_ptr<JointNameToYarpMap> jointNameToYarpMap;
    std::shared_ptr<JointNameToIndexInControlBoardMap> jointNameToIndexInControlBoardMap;
    std::shared_ptr<ControlBoardIndexLimit> controlBoardIndexLimit;
};

// BLOCK CLASS
// ===========

ModelPartitioner::ModelPartitioner()
    : pImpl{new impl()}
{}

ModelPartitioner::~ModelPartitioner() = default;

unsigned ModelPartitioner::numberOfParameters()
{
    return WBBlock::numberOfParameters() + 1;
}

bool ModelPartitioner::parseParameters(BlockInformation* blockInfo)
{
    const ParameterMetadata directionMetadata(
        ParameterType::BOOL, ParamIndex::Direction, 1, 1, "VectorToControlBoards");

    if (!blockInfo->addParameterMetadata(directionMetadata)) {
        wbtError << "Failed to store parameters metadata.";
        return false;
    }

    return blockInfo->parseParameters(m_parameters);
}

bool ModelPartitioner::configureSizeAndPorts(BlockInformation* blockInfo)
{
    if (!WBBlock::configureSizeAndPorts(blockInfo)) {
        return false;
    }

    // Get the number of the control boards
    const auto configuration = getRobotInterface()->getConfiguration();
    const int dofs = configuration.getNumberOfDoFs();
    const auto controlBoardsNumber = configuration.getControlBoardsNames().size();

    // PARAMETERS
    // ==========

    if (!ModelPartitioner::parseParameters(blockInfo)) {
        wbtError << "Failed to parse parameters.";
        return false;
    }

    bool vectorToControlBoards;
    if (!m_parameters.getParameter("VectorToControlBoards", vectorToControlBoards)) {
        wbtError << "Failed to get input parameters.";
        return false;
    }

    // INPUTS
    // ======
    //
    // VectorToControlBoards
    // ---------------------
    //
    // 1) Vector containing the data vector (1 x DoFs)
    //
    // ControlBoardsToVector
    // ---------------------
    //
    // n signals) The n ControlBoards configured from the config block
    //
    // OUTPUTS
    // =======
    //
    // VectorToControlBoards
    // ---------------------
    //
    // n signals) The n ControlBoards configured from the config block
    //
    // ControlBoardsToVector
    // ---------------------
    //
    // 1) Vector containing the data vector (1 x DoFs)
    //

    BlockInformation::IOData ioData;

    if (vectorToControlBoards) {
        // Input
        ioData.input.emplace_back(0, std::vector<int>{dofs}, DataType::DOUBLE);
        // Outputs
        for (unsigned i = 0; i < controlBoardsNumber; ++i) {
            ioData.output.emplace_back(
                ioData.output.size(), std::vector<int>{Signal::DynamicSize}, DataType::DOUBLE);
        }
    }
    else {
        // Inputs
        for (unsigned i = 0; i < controlBoardsNumber; ++i) {
            ioData.input.emplace_back(
                ioData.input.size(), std::vector<int>{Signal::DynamicSize}, DataType::DOUBLE);
        }
        // Output
        ioData.output.emplace_back(0, std::vector<int>{dofs}, DataType::DOUBLE);
    }

    if (!blockInfo->setIOPortsData(ioData)) {
        wbtError << "Failed to configure input / output ports.";
        return false;
    }

    return true;
}

bool ModelPartitioner::initialize(BlockInformation* blockInfo)
{
    if (!WBBlock::initialize(blockInfo)) {
        return false;
    }

    // Get the RobotInterface
    const auto robotInterface = getRobotInterface();

    // PARAMETERS
    // ==========

    if (!ModelPartitioner::parseParameters(blockInfo)) {
        wbtError << "Failed to parse parameters.";
        return false;
    }

    if (!m_parameters.getParameter("VectorToControlBoards", pImpl->vectorToControlBoards)) {
        wbtError << "Failed to get input parameters.";
        return false;
    }

    // CLASS INITIALIZATION
    // ====================

    pImpl->jointNameToYarpMap = robotInterface->getJointsMapString();
    pImpl->jointNameToIndexInControlBoardMap = robotInterface->getControlledJointsMapCB();

    if (!pImpl->jointNameToYarpMap) {
        wbtError << "Failed to get the joint map iDynTree <--> Yarp.";
        return false;
    }

    if (!pImpl->jointNameToIndexInControlBoardMap) {
        wbtError << "Failed to get the joint map iDynTree <--> controlledJointsIdx.";
        return false;
    }

    return true;
}

bool ModelPartitioner::terminate(const BlockInformation* blockInfo)
{
    return WBBlock::terminate(blockInfo);
}

bool ModelPartitioner::output(const BlockInformation* blockInfo)
{
    // Get the RobotInterface and the Configuration
    const auto robotInterface = getRobotInterface();
    const auto& configuration = robotInterface->getConfiguration();

    if (pImpl->vectorToControlBoards) {
        InputSignalPtr dofsSignal = blockInfo->getInputPortSignal(0);
        if (!dofsSignal) {
            wbtError << "Failed to get the input signal buffer.";
            return false;
        }

        for (unsigned ithJoint = 0; ithJoint < configuration.getNumberOfDoFs(); ++ithJoint) {
            const std::string ithJointName = configuration.getControlledJoints()[ithJoint];
            // Get the ControlBoard number the ith joint belongs
            const ControlBoardIndex& controlBoardOfJoint =
                pImpl->jointNameToYarpMap->at(ithJointName).first;
            // Get the index of the ith joint inside the controlledJoints vector relative to
            // its ControlBoard
            const JointIndexInControlBoard jointIdxInCB =
                pImpl->jointNameToIndexInControlBoardMap->at(ithJointName);

            // Get the data to forward
            OutputSignalPtr ithOutput = blockInfo->getOutputPortSignal(controlBoardOfJoint);
            if (!ithOutput->set(jointIdxInCB, dofsSignal->get<double>(ithJoint))) {
                wbtError << "Failed to set the output signal.";
                return false;
            }
        }
    }
    else {
        OutputSignalPtr dofsSignal = blockInfo->getOutputPortSignal(0);
        if (!dofsSignal) {
            wbtError << "Failed to get the input signal buffer.";
            return false;
        }

        for (unsigned ithJoint = 0; ithJoint < configuration.getNumberOfDoFs(); ++ithJoint) {
            const std::string ithJointName = configuration.getControlledJoints()[ithJoint];
            // Get the ControlBoard number the ith joint belongs
            const ControlBoardIndex& controlBoardOfJoint =
                pImpl->jointNameToYarpMap->at(ithJointName).first;
            // Get the index of the ith joint inside the controlledJoints vector relative to
            // its ControlBoard
            const JointIndexInControlBoard jointIdxInCB =
                pImpl->jointNameToIndexInControlBoardMap->at(ithJointName);

            // Get the data to forward
            InputSignalPtr ithInput = blockInfo->getInputPortSignal(controlBoardOfJoint);
            if (!dofsSignal->set(ithJoint, ithInput->get<double>(jointIdxInCB))) {
                wbtError << "Failed to set the output signal.";
                return false;
            }
        }
    }
    return true;
}