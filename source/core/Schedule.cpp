//
//  Schedule.cpp
//  MNN
//
//  Created by MNN on 2018/07/30.
//  Copyright © 2018, Alibaba Group Holding Limited
//

#include "core/Schedule.hpp"
#include <algorithm>
#include <iterator>
#include <set>
#include <unordered_map>
#include "core/DirectedAcyclicGraph.hpp"
#include "core/Macro.h"
#include "core/RuntimeFactory.hpp"
#include "core/TensorUtils.hpp"
#include "shape/SizeComputer.hpp"
#include "utils/InitNet.hpp"
//#define MNN_OPEN_TIME_TRACE
#include <MNN/AutoTime.hpp>
//#define MNN_AUTO_CHECK_COST
namespace MNN {

class OpNodeDef : public NodeDef<Op*> {
public:
    OpNodeDef(Op* op) {
        this->op = op;
    }

public:
    std::shared_ptr<Node<Op*>> makeNode() override {
        std::shared_ptr<Node<Op*>> ptr = std::make_shared<Node<Op*>>();
        ptr->setData(this->op);
        return ptr;
    }

private:
    Op* op {nullptr};
};

MNNForwardType Schedule::getApprociateType(const ScheduleConfig& config) {
    MNNForwardType type = config.type;
    // FIXME: Support Auto determine
    if (MNN_FORWARD_AUTO == config.type) {
        // Search Backend Exclude MNN_FORWARD_CPU
        for (int i = 1; i < MNN_FORWARD_ALL; ++i) {
            if (MNNGetExtraRuntimeCreator((MNNForwardType)i) != nullptr) {
                type = (MNNForwardType)i;
                break;
            }
        }
    }
    auto creator = MNNGetExtraRuntimeCreator(type);
    if (nullptr == creator) {
        MNN_PRINT("Can't Find type=%d backend, use %d instead\n", type, config.backupType);
        type = config.backupType;
    }
    return type;
}

static bool _setUpTensorInfo(std::vector<std::shared_ptr<Tensor>>& allTensors, const Net* net) {
    bool valid    = true;
    auto& tensors = allTensors;
    tensors.resize(net->tensorName()->size());
    if (net->usage() == Usage_INFERENCE_STATIC) {
        // static model will set all tensors' shape
        auto describes = net->extraTensorDescribe();
        std::vector<const TensorDescribe*> des(tensors.size());
        for (int i = 0; i < describes->size(); i++) {
            int index  = describes->GetAs<TensorDescribe>(i)->index();
            des[index] = describes->GetAs<TensorDescribe>(i);
        }
        for (int i = 0; i < tensors.size(); ++i) {
            auto blob = des[i]->blob();
            if (auto idims = blob->dims()) {
                tensors[i].reset(new Tensor(idims->size()));
                auto& tb = tensors[i]->buffer();
                for (int d = 0; d < idims->size(); d++) {
                    tb.dim[d].extent = idims->Get(d);
                }
            } else {
                tensors[i].reset(new Tensor(1));
            }
            tensors[i]->setType(blob->dataType());
        }
        for (int i = 0; i < tensors.size(); ++i) {
            auto blob                                                   = des[i]->blob();
            TensorUtils::getDescribe(tensors[i].get())->dimensionFormat = blob->dataFormat();
            if (auto regions = des[i]->regions()) {
                auto& regs = TensorUtils::getDescribe(tensors[i].get())->regions;
                TensorUtils::getDescribe(tensors[i].get())->memoryType = Tensor::InsideDescribe::MemoryType::MEMORY_VIRTUAL;
                regs.reserve(regions->size());
                for (int r = 0; r < regions->size(); r++) {
                    auto region = regions->GetAs<Region>(r);
                    Tensor::InsideDescribe::Region reg;
                    reg.origin     = tensors[region->origin()].get();
                    reg.src.offset = region->src()->offset();
                    reg.dst.offset = region->dst()->offset();
                    for (int d = 0; d < 3; d++) {
                        reg.size[d]       = region->size()->data()[d];
                        reg.src.stride[d] = region->src()->stride()->data()[d];
                        reg.dst.stride[d] = region->dst()->stride()->data()[d];
                    }
                    regs.emplace_back(std::move(reg));
                }
            }
        }
        for (int opIndex = 0; opIndex < net->oplists()->size(); ++opIndex) {
            auto op = net->oplists()->GetAs<Op>(opIndex);
            if (OpType_Const == op->type()) {
                MNN_ASSERT(nullptr != op->outputIndexes());
                auto index                                            = op->outputIndexes()->data()[0];
                TensorUtils::getDescribe(tensors[index].get())->usage = Tensor::InsideDescribe::Usage::CONSTANT;
            }
        }
    } else {
        // Dynamic Model just set input tensor's shape
        valid = initTensors(tensors, net);
    }
    return valid;
}

static int _findOpPosition(const std::string& opName, const Net* net) {
    for (int i = 0; i < net->oplists()->size(); ++i) {
        auto op = net->oplists()->GetAs<Op>(i);
        if (opName == op->name()->str()) {
            return i;
        }
    }
    return -1;
}

static bool _validateOp(const Op* op) {
    if (nullptr == op->inputIndexes() && nullptr == op->outputIndexes()) {
        return false;
    }
    if (nullptr == op->name()) {
        return false;
    }
    return true;
}

static std::vector<Op*> generateOneSchedulePath(const Net* net, const int begin, const int end,
                                           const std::vector<std::shared_ptr<Tensor>>& allTensors) {
    std::vector<Op*> oplists;
    for (int i = begin; i < end; ++i) {
        auto op = net->oplists()->GetAs<Op>(i);
        if (op->type() == OpType_Input || !_validateOp(op)) {
            continue;
        }
        oplists.emplace_back(const_cast<Op*>(op));
    }
    return oplists;
}

static std::vector<std::vector<Op*>> generateSchedulePath(const Net* net, const ScheduleConfig& configs,
                                                          const std::vector<std::shared_ptr<Tensor>>& allTensors) {
    std::vector<std::vector<Op*>> oplists;
    std::vector<std::string> inputs(configs.path.inputs);
    std::vector<std::string> outputs(configs.path.outputs);
    auto maxSize = std::max(inputs.size(), outputs.size());
    inputs.resize(maxSize);
    outputs.resize(maxSize);

    for (int i = 0; i < inputs.size(); i++) {
        std::string in  = inputs[i];
        std::string out = outputs[i];
        int start  = 0;
        int end    = net->oplists()->size();
        if (in.length() > 0) {
            auto pos = _findOpPosition(in, net);
            if (-1 == pos) {
                MNN_PRINT("Can't find %s op as start op\n", in.c_str());
            } else {
                start = pos;
            }
        }
        if (out.length() > 0) {
            auto pos = _findOpPosition(out, net);
            if (-1 == pos) {
                MNN_PRINT("Can't find %s op as end op\n", out.c_str());
            } else {
                end = pos + 1;
            }
        }
        if (start > end) {
            MNN_PRINT("op order incorrect end op '%s' before begin op '%s',please check!\n", out.c_str(), in.c_str());
        } else {
            std::vector<Op*> path = generateOneSchedulePath(net, start, end, allTensors);
            oplists.emplace_back(path);
        }
    }

    return oplists;
}

static void generateScheduleGraph(std::vector<const Op*>& ops, const Net* net, const ScheduleConfig& configs,
                                  const std::vector<std::shared_ptr<Tensor>>& allTensors) {
    if (configs.path.inputs.empty() && configs.path.outputs.empty()) {
        // Use Default Linear schedule
        ops.clear();
        ops.reserve(net->oplists()->size());
        for (int i = 0; i < net->oplists()->size(); ++i) {
            auto op = net->oplists()->GetAs<Op>(i);
            if (op->type() != OpType_Input) {
                ops.emplace_back(op);
            }
        }
        return;
    }
    std::vector<std::vector<Op*>> paths = generateSchedulePath(net, configs, allTensors);

    std::unique_ptr<DirectedAcyclicGraph<Op*>> graph(new DirectedAcyclicGraph<Op*>());

    // add Node
    std::unordered_map<Op*, std::shared_ptr<Node<Op*>>> opMaps;
    for (std::vector<Op*> path : paths) {
        for (Op* op : path) {
            if (opMaps.find(op) == opMaps.end()) {
                OpNodeDef def(op);
                std::shared_ptr<Node<Op*>> n = graph->AddNode(def);
                opMaps.emplace(op, n);
            }
        }
    }

    // add edges
    for (std::vector<Op*> path : paths) {
        std::shared_ptr<Node<Op*>> pre = nullptr;
        for (Op* op : path) {
            std::shared_ptr<Node<Op*>> n = opMaps[op];
            if (nullptr == pre) {
                pre = n;
            } else {
                graph->AddEdge(pre, n);
                pre = n;
            }
        }
    }
    ops.clear();
    std::vector<std::shared_ptr<Node<Op*>>> order;
    if (graph->GetPostOrder(order)) {
        for (std::shared_ptr<Node<Op*>> n : order) {
            ops.emplace_back(n->getData());
        }
    } else {
        MNN_PRINT("op graph have cycle,schedule failed\n");
    }
}

static std::vector<Schedule::PipelineInfo> _scheduleUnit(const Net* net, const ScheduleConfig& configs,
                                                         const std::vector<std::shared_ptr<Tensor>>& allTensors) {
    std::vector<Schedule::PipelineInfo> oplists;
    std::vector<const Op*> ops;
    generateScheduleGraph(ops, net, configs, allTensors);
    initPipelineInfosFromOps(oplists, ops, allTensors);
    return oplists;
}

Schedule::ScheduleInfo Schedule::schedule(const Net* net, const std::vector<ScheduleConfig>& configs) {
    std::vector<std::shared_ptr<Tensor>> allTensors;

    ScheduleInfo schedule;
    if (nullptr == net->oplists()) {
        MNN_PRINT("Error net for schedule\n");
        return schedule;
    }
    bool valid              = _setUpTensorInfo(allTensors, net);
    schedule.validForResize = valid;

    std::vector<std::pair<Backend::Info, std::vector<Schedule::PipelineInfo>>> result;

    for (auto& config : configs) {
        Backend::Info compute;
        compute.type      = getApprociateType(config);
        compute.numThread = config.numThread;
        compute.user      = config.backendConfig;
        auto oplists      = _scheduleUnit(net, config, allTensors);
        result.emplace_back(std::make_pair(compute, std::move(oplists)));
    }

    schedule.pipelineInfo = std::move(result);

    // get all used op's output, drop unused op, won't change op order. always insert all Input Ops
    std::vector<const Op*> oplists;
    {
        for (std::pair<Backend::Info, std::vector<Schedule::PipelineInfo>>& pipeline : schedule.pipelineInfo) {
            for (auto& info : pipeline.second) {
                oplists.push_back(info.op);
            }
        }
    }
    // set tensors' input/output usage by oplists info
    setInputOutputForOps(allTensors, oplists, net->usage() == Usage_INFERENCE_STATIC);

    // add output index by config info and outputName
    std::unordered_map<std::string, int> tensorNameIndexMap;
    for (int i = 0; i < net->tensorName()->size(); ++i) {
        tensorNameIndexMap[net->tensorName()->Get(i)->str()] = i;
    }
    for (auto& config : configs) {
        for (const auto& name : config.saveTensors) {
            auto iter = tensorNameIndexMap.find(name);
            if (iter != tensorNameIndexMap.end()) {
                auto t = allTensors[iter->second].get();
                if (TensorUtils::getDescribe(t)->usage == Tensor::InsideDescribe::Usage::NORMAL) {
                    TensorUtils::getDescribe(t)->usage = Tensor::InsideDescribe::Usage::OUTPUT;
                } else {
                    schedule.outputTensor.insert(
                               std::make_pair(net->tensorName()->GetAsString(iter->second)->c_str(), t));
                }
            } else {
                MNN_PRINT("Bad outputname: %s\n", name.c_str());
            }
        }
    }
    if (net->outputName()) {
        for (int i = 0; i < net->outputName()->size(); ++i) {
            std::string name = net->outputName()->Get(i)->str();
            auto iter = tensorNameIndexMap.find(name);
            if (iter != tensorNameIndexMap.end()) {
                auto t = allTensors[iter->second].get();
                if (TensorUtils::getDescribe(t)->usage == Tensor::InsideDescribe::Usage::NORMAL) {
                    TensorUtils::getDescribe(t)->usage = Tensor::InsideDescribe::Usage::OUTPUT;
                } else {
                    schedule.outputTensor.insert(
                               std::make_pair(net->tensorName()->GetAsString(iter->second)->c_str(), t));
                }
            }
        }
    }
    // add input/output tensor to schedule's input/output
    for (int index = 0; index < allTensors.size(); index++) {
        auto t = allTensors[index].get();
        auto usage = TensorUtils::getDescribe(t)->usage;
        if (usage == Tensor::InsideDescribe::Usage::INPUT) {
            schedule.inputTensors.insert(std::make_pair(net->tensorName()->GetAsString(index)->c_str(), t));
        }
        if (usage == Tensor::InsideDescribe::Usage::OUTPUT) {
            schedule.outputTensor.insert(
                       std::make_pair(net->tensorName()->GetAsString(index)->c_str(), t));
        }
    }
    // move tensors to schedule
    for (auto& t : allTensors) {
        schedule.allTensors.emplace_back(std::make_pair(0, std::move(t)));
    }
    return schedule;
}
} // namespace MNN
