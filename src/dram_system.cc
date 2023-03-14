#include "dram_system.h"

#include <assert.h>

namespace dramsim3 {

// alternative way is to assign the id in constructor but this is less
// destructive
int BaseDRAMSystem::total_channels_ = 0;

BaseDRAMSystem::BaseDRAMSystem(Config &config, const std::string &output_dir,
                               std::function<void(uint64_t)> read_callback,
                               std::function<void(uint64_t)> write_callback)
    : read_callback_(read_callback),
      write_callback_(write_callback),
      last_req_clk_(0),
      config_(config),
      timing_(config_),
#ifdef THERMAL
      thermal_calc_(config_),
#endif  // THERMAL
      clk_(0) {
    total_channels_ += config_.channels;

    int banks =  config_.ranks * config_.bankgroups * config_.banks_per_group;
    for (auto i = 0; i < config_.channels; i++) {
        auto chan_occupancy =
            std::vector<bool>(banks, false);
        bank_occupancy_.push_back(chan_occupancy);
    }

#ifdef ADDR_TRACE
    std::string addr_trace_name = config_.output_prefix + "addr.trace";
    address_trace_.open(addr_trace_name);
#endif
}

int BaseDRAMSystem::GetChannel(uint64_t hex_addr) const {
    hex_addr >>= config_.shift_bits;
    return (hex_addr >> config_.ch_pos) & config_.ch_mask;
}

void BaseDRAMSystem::PrintEpochStats() {
    // first epoch, print bracket
    if (clk_ - config_.epoch_period == 0) {
        std::ofstream epoch_out(config_.json_epoch_name, std::ofstream::out);
        epoch_out << "[";
    }
    for (size_t i = 0; i < ctrls_.size(); i++) {
        ctrls_[i]->PrintEpochStats();
        std::ofstream epoch_out(config_.json_epoch_name, std::ofstream::app);
        epoch_out << "," << std::endl;
    }
#ifdef THERMAL
    thermal_calc_.PrintTransPT(clk_);
#endif  // THERMAL
    return;
}

void BaseDRAMSystem::PrintStats() {
    // Finish epoch output, remove last comma and append ]
    std::ofstream epoch_out(config_.json_epoch_name, std::ios_base::in |
                                                         std::ios_base::out |
                                                         std::ios_base::ate);
    epoch_out.seekp(-2, std::ios_base::cur);
    epoch_out.write("]", 1);
    epoch_out.close();

    std::ofstream json_out(config_.json_stats_name, std::ofstream::out);
    json_out << "{";

    // close it now so that each channel can handle it
    json_out.close();
    for (size_t i = 0; i < ctrls_.size(); i++) {
        ctrls_[i]->PrintFinalStats();
        if (i != ctrls_.size() - 1) {
            std::ofstream chan_out(config_.json_stats_name, std::ofstream::app);
            chan_out << "," << std::endl;
        }
    }
    json_out.open(config_.json_stats_name, std::ofstream::app);
    json_out << "}";

#ifdef THERMAL
    thermal_calc_.PrintFinalPT(clk_);
#endif  // THERMAL
}

void BaseDRAMSystem::ResetStats() {
    for (size_t i = 0; i < ctrls_.size(); i++) {
        ctrls_[i]->ResetStats();
    }
}

void BaseDRAMSystem::RegisterCallbacks(
    std::function<void(uint64_t)> read_callback,
    std::function<void(uint64_t)> write_callback) {
    // TODO this should be propagated to controllers
    read_callback_ = read_callback;
    write_callback_ = write_callback;
}

JedecDRAMSystem::JedecDRAMSystem(Config &config, const std::string &output_dir,
                                 std::function<void(uint64_t)> read_callback,
                                 std::function<void(uint64_t)> write_callback)
    : BaseDRAMSystem(config, output_dir, read_callback, write_callback) {
    if (config_.IsHMC()) {
        std::cerr << "Initialized a memory system with an HMC config file!"
                  << std::endl;
        AbruptExit(__FILE__, __LINE__);
    }

    ctrls_.reserve(config_.channels);
    for (auto i = 0; i < config_.channels; i++) {
#ifdef THERMAL
        ctrls_.push_back(new Controller(i, config_, timing_, thermal_calc_));
#else
        ctrls_.push_back(new Controller(i, config_, timing_));
#endif  // THERMAL
    }
}

JedecDRAMSystem::~JedecDRAMSystem() {
    for (auto it = ctrls_.begin(); it != ctrls_.end(); it++) {
        delete (*it);
    }
}


bool JedecDRAMSystem::WillAcceptTransaction() const {
    return pim_trans_queue_.size() < pim_trans_queue_depth_;
}

bool JedecDRAMSystem::AddTransaction(uint64_t hex_addr) {
// Record trace - Record address trace for debugging or other purposes
#ifdef ADDR_TRACE
    address_trace_ << std::hex << hex_addr << std::dec << " "
                   << "PIM " << clk_ << std::endl;
#endif

    bool ok = WillAcceptTransaction();

    assert(ok);
    if (ok) {
        Transaction trans = Transaction(hex_addr);
        trans.is_pim = true;

        uint64_t tempA, tempB, address;
        address = trans.addr;

        unsigned tensor, confTypeV, isConfiguredV, confTypeH, isConfiguredH, rowAddress;

        tempA = address;
        address = address >> 3;
        tempB = address << 3;
        tensor = tempA ^ tempB;

        tempA = address;
        address = address >> 2;
        tempB = address << 2;
        confTypeV = tempA ^ tempB;

        tempA = address;
        address = address >> 8;
        tempB = address << 8;
        isConfiguredV = tempA ^ tempB;

        tempA = address;
        address = address >> 1;
        tempB = address << 1;
        confTypeH = tempA ^ tempB;

        tempA = address;
        address = address >> 2;
        tempB = address << 2;
        isConfiguredH = tempA ^ tempB;

        rowAddress = address;

        size_t numChunksV = std::pow(2, confTypeV); // 2 == NUM_BANKS / max_num_chunks
        int chunk_sizeV = (config_.ranks * config_.bankgroups * config_.banks) / numChunksV;

        size_t numChunksH = std::pow(2, confTypeH);
        int chunk_sizeH = config_.channels / numChunksH;


        for (size_t i=0; i<numChunksV; i++)
        {
            if (isConfiguredV & (1 << i))
            {
                if (tensor == 0) // input
                {

                    trans.targetBanks.push_back(i*chunk_sizeV);
                }

                else if (tensor == 1) // weight
                {
                    for (size_t j=0; j<chunk_sizeV; j++)
                        trans.targetBanks.push_back(i*chunk_sizeV + j);
                }

                else if (tensor == 2) // output
                {
                    trans.targetBanks.push_back(i*chunk_sizeV + 1);
                }
            }
        }

        trans.row_addr = rowAddress;

        pim_trans_queue_.push_back(trans);

    }
    last_req_clk_ = clk_;
    return ok;
}

bool JedecDRAMSystem::WillAcceptTransaction(uint64_t hex_addr,
                                            bool is_write) const {
    int channel = GetChannel(hex_addr);
    return ctrls_[channel]->WillAcceptTransaction(hex_addr, is_write);
}

bool JedecDRAMSystem::AddTransaction(uint64_t hex_addr, bool is_write) {
// Record trace - Record address trace for debugging or other purposes
#ifdef ADDR_TRACE
    address_trace_ << std::hex << hex_addr << std::dec << " "
                   << (is_write ? "WRITE " : "READ ") << clk_ << std::endl;
#endif

    int channel = GetChannel(hex_addr);
    bool ok = ctrls_[channel]->WillAcceptTransaction(hex_addr, is_write);

    assert(ok);
    if (ok) {
        Transaction trans = Transaction(hex_addr, is_write);
        ctrls_[channel]->AddTransaction(trans);
    }
    last_req_clk_ = clk_;
    return ok;
}

void JedecDRAMSystem::ClockTick() {
    for (size_t i = 0; i < ctrls_.size(); i++) {
        // look ahead and return earlier
        while (true) {
            auto pair = ctrls_[i]->ReturnDoneTrans(clk_);
            if (pair.second == 1) {
                write_callback_(pair.first);
            } else if (pair.second == 0) {
                read_callback_(pair.first);
            } else {
                break;
            }
        }
    }

    //TODO refresh check
    //lookup refresh_ in each controller to check refresh countdown
    //if countdown is lower than pim delay, pause issuing pim commands until refresh is done over all ranks

    //TODO npu signals
    bool weight_fetching = false;
    bool input_sending = false;
    int tensor_bitwidth = 3;

    if (!pim_trans_queue_.empty()) {
        for (auto& it : pim_trans_queue_) {
            if (it.active && it.col_num >= 0) {
                uint64_t tensor = it.addr ^ ((it.addr >> tensor_bitwidth) << tensor_bitwidth);
                if (tensor == 0 && weight_fetching) {
                    assert(col_num == 0);
                    // not send cmd
                }
                else if (tensor == 1 && input_sending) {
                    assert(col_num == 0);
                    // not send cmd
                }
                else {

                    CommandType type;
                    if (!it.active) {
                        type = CommandType::ACTIVATE;
                        assert(it.col_num == 0);
                    }
                    else if (it.col_num == 32) {
                        type = CommandType::PRECHARGE;
                    }
                    else if (tensor == 2) {
                        type = CommandType::WRITE;
                    }
                    else {
                        type = CommandType::READ;
                    }

                    if (isIssuable_pim(it, type)) {

                        switch (type) {
                            case CommandType::ACTIVATE:
                                break;
                            case CommandType::PRECHARGE:
                                it.col_num = 0;
                                break;
                            case CommandType::WRITE:
                            case CommandType::READ:
                                it.col_num++;
                                break;
                            default:
                                std::cout << (int) type << it << std::endl;
                                AbruptExit(__FILE__, __LINE__);
                        }

                        for (auto& itC : it.targetChans) {
                            for (auto& itB : it.targetBanks) {
                                bank_occupancy_[itC][itB] = true;

                                // TODO Address scheme
                                Address addr = Address((int) itC, 0, 0, (int) itB, (int) it.row_addr,  (int) it.col_num);
                                Command cmd = Command(type, addr, it.addr);
                                Command ready_cmd = ctrls_[itC]->GetReadyCommand(cmd, clk_);

                                assert(cmd.cmd_type == ready_cmd.cmd_type);
                                assert(ready_cmd.IsValid());

                                ctrls_[itC]->pim_cmds_.push_back(ready_cmd);
                            }
                        }
                    }
                }

            }
        }

    }

    for (size_t i = 0; i < ctrls_.size(); i++) {
        ctrls_[i]->ClockTick();
    }
    clk_++;

    if (clk_ % config_.epoch_period == 0) {
        PrintEpochStats();
    }
    return;
}

bool JedecDRAMSystem::isIssuable_pim(Transaction trans, CommandType type) {
    for (auto& itC : trans.targetChans) {
        // We do not need to add more loops here since only these two dimensions are orthogonal to each other
        for (auto& itB : trans.targetBanks) {
            // TODO how to set rank, bankgroup, and column address (device width)?
            Address addr = Address((int) itC, 0, 0, (int) itB, (int) trans.row_addr,  (int) trans.col_num);
            Command cmd = Command(type, addr, trans.addr);
            Command ready_cmd = ctrls_[itC]->GetReadyCommand(cmd, clk_);
            if (!ready_cmd.IsValid() || bank_occupancy_[itC][itB]) return false;
        }
    }
    return true;
}



IdealDRAMSystem::IdealDRAMSystem(Config &config, const std::string &output_dir,
                                 std::function<void(uint64_t)> read_callback,
                                 std::function<void(uint64_t)> write_callback)
    : BaseDRAMSystem(config, output_dir, read_callback, write_callback),
      latency_(config_.ideal_memory_latency) {}

IdealDRAMSystem::~IdealDRAMSystem() {}

bool IdealDRAMSystem::AddTransaction(uint64_t hex_addr, bool is_write) {
    auto trans = Transaction(hex_addr, is_write);
    trans.added_cycle = clk_;
    infinite_buffer_q_.push_back(trans);
    return true;
}

void IdealDRAMSystem::ClockTick() {
    for (auto trans_it = infinite_buffer_q_.begin();
         trans_it != infinite_buffer_q_.end();) {
        if (clk_ - trans_it->added_cycle >= static_cast<uint64_t>(latency_)) {
            if (trans_it->is_write) {
                write_callback_(trans_it->addr);
            } else {
                read_callback_(trans_it->addr);
            }
            trans_it = infinite_buffer_q_.erase(trans_it++);
        }
        if (trans_it != infinite_buffer_q_.end()) {
            ++trans_it;
        }
    }

    clk_++;
    return;
}

}  // namespace dramsim3
