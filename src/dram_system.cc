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

    int banks =  config_.ranks * config_.bankgroups * config_.banks_per_group;
    for (auto i = 0; i < config_.channels; i++) {
        auto chan_occupancy =
            std::vector<bool>(banks, false);
        bank_occupancy_.push_back(chan_occupancy);
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

    //lookup refresh_ in each controller to check refresh countdown
    //if countdown is lower than pim delay, pause issuing pim commands until refresh is done over all ranks
    bool wait_refresh = false;
    for (size_t i=0; i<ctrls_.size(); i++) {
        if (ctrls_[i]->pim_refresh_coming()) {
            wait_refresh = true;
            std::cout<<clk_ << "\tWait Refresh\n";
        }
    }

    //TODO npu signals
    bool weight_fetching = false;
    bool input_sending = false;
    int tensor_bitwidth = 3;
    bool input_trans_found = false;
    bool weight_trans_found = false;
    bool output_trans_found = false;


    if (pim_trans_queue_.empty()) std::cout<<clk_ << "\tEmpty PIM trans queue\n";

    if (!pim_trans_queue_.empty()) { // wait_refresh: when sending an activation cmd. no configured var
        std::cout<<clk_<<"\tTransaction Queue size: " << pim_trans_queue_.size() << '\n';
        for (auto& it : pim_trans_queue_) {
            uint64_t address = it.addr;
            int cut_no;
            int bw_cutNo = 4;
            int bw_vcuts = 2;
            int bw_hcuts = 1;
            int bw_Mtile = 4;
            int bw_kernelSize = 5;
            int bw_stride = 5;
            int bw_dimValue = 32;
            int bw_baseRow = 22;
            int bw_loadType = 2;

            if (it.addr & 1) { // computing
                address = address >> 1;
                cut_no = address & ((1 << bw_cutNo)-1);

                bool configured = M[cut_no] != 0 && N[cut_no] != 0 && K[cut_no] != 0;
                if (!configured) break;

                in_pim[cut_no] = true;

            }
            else if (it.addr & (3 << 5)) { // cutting


                base_rows_w.clear();
                base_rows_in.clear();
                base_rows_out.clear();
                M.clear();
                N.clear();
                K.clear();
                opcnt_in.clear();
                opcnt_w.clear();
                opcnt_out.clear();

                address = address >> 1 >> 4 >> 2; // trans_type, cut_no, loadType
                vcuts = 1 << (address & ((1<<bw_vcuts)-1));
                address = address >> bw_vcuts;
                hcuts = 1 << (address & ((1<<bw_hcuts)-1));
                address = address >> bw_hcuts;
                M_tile = 1 << (address & ((1<<bw_Mtile)-1));
                address = address >> bw_Mtile;
                vcuts_next = 1 << (address & ((1<<bw_vcuts)-1));
                address = address >> bw_vcuts;
                hcuts_next = 1 << (address & ((1<<bw_hcuts)-1));
                address = address >> bw_hcuts;
                kernel_size = address & ((1<<bw_kernelSize)-1);
                address = address >> bw_kernelSize;
                stride = address & ((1<<bw_stride)-1);

                int cuts = vcuts * hcuts;
                base_rows_w.assign(cuts, 0);
                base_rows_in.assign(cuts, 0);
                base_rows_out.assign(cuts, 0);
                M.assign(cuts, 0);
                N.assign(cuts, 0);
                K.assign(cuts, 0);
                opcnt_in.assign(cuts, 0);
                opcnt_w.assign(cuts, 0);
                opcnt_out.assign(cuts, 0);
                in_pim.assign(cuts, false);
                break;

            }
            else { // loading
                address = address >> 1;
                cut_no = address & ((1 << bw_cutNo)-1);
                address = address >> 4;
                int loadType = address & ((1<<bw_loadType)-1);
                address = address >> bw_loadType;
                int dim_value = address & ((1<<bw_dimValue)-1);
                address = address >> bw_dimValue;
                uint64_t base_row = address & ((1<<bw_baseRow)-1);
                address = address >> bw_baseRow;
                switch(loadType) {
                    case 0: // M, Input
                        base_rows_in[cut_no] = base_row;
                        M[cut_no] = dim_value;
                        break;
                    case 1: // K, weight
                        base_rows_w[cut_no] = base_row;
                        K[cut_no] = dim_value;
                        break;
                    case 2: // N, output
                        base_rows_out[cut_no] = base_row;
                        N[cut_no] = dim_value;
                        break;
                    default:
                        std::cerr << "Invalid load type!"
                                  << std::endl;
                        AbruptExit(__FILE__, __LINE__);
                        break;
                }
                break;

            }



            if (!it.active) {
                std::cout<<clk_<<"\tTransaction Inactive: " << it << "\t" << it.col_num << "\t" << it.end_col << '\n';
                continue;
            }
            std::cout<<clk_<<"\tTransaction Active: " << it << "\t" << it.col_num << "\t" << it.end_col << '\n';

            uint64_t tensor = it.addr ^ ((it.addr >> tensor_bitwidth) << tensor_bitwidth);
            bool is_config = false;



            bool issuable = true;
            std::cout<<(int)it.col_num<<": col_num\n";
            // Input, Weight Dependency
            if (tensor == 0 && weight_fetching) {
                std::cout<<clk_<<"\tWeight Dependency: "<<it<<'\n';
                assert(it.col_num == 0);
                issuable = false; //not send cmd?
            }
            else if (tensor == 1 && input_sending) {
                std::cout<<clk_<<"\tInput Dependency: "<<it<<'\n';

                issuable = false; // not send cmd?
            }

            // For bank interleaving
            bool ex_pending = (input_trans_found && tensor == 0) ||
                              (weight_trans_found && tensor == 1) ||
                              (output_trans_found && tensor == 2);

            // TODO when PIM_E
            if (tensor == 0) input_trans_found = true;
            else if (tensor == 1) weight_trans_found = true;
            else output_trans_found = true;


            // TODO: R/W_PRECHARGE when col_num is 31 for energy saving
            CommandType type;
            if (tensor == 2) {
                type = CommandType::WRITE;
            }
            else {
                type = CommandType::READ;
            }

            Command ready_cmd_temp = GetReadyCommandPIM(it, type);

            bool ReadOrWrite = ready_cmd_temp.cmd_type == type;

            // TODO: more restrictive false to give tighter timing for energy saving
            if (ex_pending && ReadOrWrite) {
                issuable = false;
                std::cout<<clk_<<"\tintra Dependency: "<<it<<'\n';
            }

            if (!ready_cmd_temp.IsValid()) std::cout<<clk_<<"\tNot To be issued: " << ready_cmd_temp << '\n';
            if (ready_cmd_temp.IsValid() && issuable) {
                std::cout<<clk_<<"\tTo be issued: "<<ready_cmd_temp<<'\n';
                if (ReadOrWrite) {
                    it.col_num++;
                }

                for (auto& itC : it.targetChans) {
                    for (auto& itB : it.targetBanks) {

                        // TODO Address scheme
                        Address addr = Address((int) itC, 0, 0, (int) itB, (int) it.row_addr,  (int) it.col_num);

                        Command cmd = Command(type, addr, it.addr);
                        Command ready_cmd = ctrls_[itC]->GetReadyCommand(cmd, clk_);
                        assert(ready_cmd.IsValid());
                        assert(ready_cmd.cmd_type == ready_cmd_temp.cmd_type);


                        if (ready_cmd_temp.cmd_type == CommandType::ACTIVATE)
                            bank_occupancy_[itC][itB] = true;
                        else
                            assert(bank_occupancy_[itC][itB]);

                        ctrls_[itC]->pim_cmds_.push_back(ready_cmd);
                    }
                }
            }

        }
        // bank_occupancy_ free at last R/W
        for (auto it = pim_trans_queue_.begin(); it != pim_trans_queue_.end(); ) {
            if (it->active && it->col_num == it->end_col) {
                std::cout<<clk_<<"\tTransaction clean?: " << *it << "\t" << it->col_num << "\t" << it->end_col << '\n';
                for (auto& itC : it->targetChans) {
                    for (auto& itB : it->targetBanks) {
                        bank_occupancy_[itC][itB] = false;
                    }
                }
                std::cout<<clk_<<"\tErase transaction "<<*it<<'\n';
                it = pim_trans_queue_.erase(it);
            }
            else
                it++;
        }

    }

    for (size_t i = 0; i < ctrls_.size(); i++) {
        ctrls_[i]->ClockTick();
    }

    // Schedule PIM Transactions
    for (auto it = pim_trans_queue_.begin(); it != pim_trans_queue_.end(); it++) {
        if (it->active) continue;

        uint64_t tempA, tempB, address;
        address = it->addr;

        unsigned tensor, confTypeV, isConfiguredV, confTypeH, isConfiguredH, rowAddress, weightOffset;

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

        tempA = address;
        address = address >> 1;
        tempB = address << 1;
        weightOffset = tempA ^ tempB;

        rowAddress = address;

        size_t numChunksV = std::pow(2, confTypeV); // 2 == NUM_BANKS / max_num_chunks
        int chunk_sizeV = (config_.ranks * config_.banks) / numChunksV;

        size_t numChunksH = std::pow(2, confTypeH);
        int chunk_sizeH = config_.channels / numChunksH;

        for (size_t i=0; i<numChunksV; i++)
        {
            if (isConfiguredV & (1 << i))
            {
                std::cout<<"targetBanks added\n";
                if (tensor == 0) // input
                {
                    it->targetBanks.push_back(i*chunk_sizeV);
                }

                else if (tensor == 1) // weight
                {
                    for (size_t j=0; j<chunk_sizeV/2; j++)
                        it->targetBanks.push_back(i*chunk_sizeV + 2*j); // even banks
                }

                else if (tensor == 2) // output
                {
                    it->targetBanks.push_back(i*chunk_sizeV + 1);
                }
            }
        }

        for (size_t i=0; i<numChunksH; i++)
        {
            if (isConfiguredH & (1 << i))
            {
                std::cout<<"targetChans added\n";
                for (size_t j=0; j<chunk_sizeH; j++){
                        it->targetChans.push_back(i*chunk_sizeH + j); // even banks
                }
            }
        }

        int w_reads_per_tile = 128/((config_.ranks*config_.bankgroups*config_.banks)/2); // 16 when peX==128 and bandX==16

        int col_num;
        if (tensor == 1 && w_reads_per_tile < 32)
            col_num = weightOffset * w_reads_per_tile;
        else col_num = 0;

        int end_col = (tensor != 1 || w_reads_per_tile > 32) ? 32: (weightOffset+1) * w_reads_per_tile;
        std::cout<<end_col<<": col_end\n";

        it->is_pim = true;
        it->col_num = col_num;
        it->row_addr = rowAddress;
        it->end_col = end_col;
        it->active = true;

        break; // activate one transaction per cycle
    }
    clk_++;

    if (clk_ % config_.epoch_period == 0) {
        PrintEpochStats();
    }
    return;
}

Command JedecDRAMSystem::GetReadyCommandPIM(Transaction trans, CommandType type) {
    bool first = true;
    bool sameornot = false;
    Command ready_cmd = Command();
    if (trans.targetChans.empty() || trans.targetBanks.empty()) std::cout<<"empty target Chans, Banks\t"<<trans<<'\n';
    for (auto& itC : trans.targetChans) {
        // We do not need to add more loops here since only these two dimensions are orthogonal to each other
        for (auto& itB : trans.targetBanks) {
            // TODO how to set rank, bankgroup, and column address (device width)?
            Address addr = Address((int) itC, 0, 0, (int) itB, (int) trans.row_addr,  (int) trans.col_num);
            Command cmd = Command(type, addr, trans.addr);
            std::cout<<clk_<<"\tWant To issue: " << cmd << '\n';
            ready_cmd = ctrls_[itC]->GetReadyCommand(cmd, clk_);
            if (first) {
                first = false;
                sameornot = cmd.cmd_type == ready_cmd.cmd_type;
            }
            else {
                if (sameornot != (cmd.cmd_type == ready_cmd.cmd_type)) {
                    std::cout <<clk_<< "\tSame or not fail:\n" << cmd << '\n' << ready_cmd << '\n';
                    return Command();
                }
            }
            if (!ready_cmd.IsValid() || bank_occupancy_[itC][itB]) {
                std::cout << clk_<<"\tGetReadyCommandPIM fail(" << ready_cmd << ") - Bank occupancy " << itC << itB << ": " << bank_occupancy_[itC][itB] << '\n';
                return Command();
            }
        }
    }
    return ready_cmd;
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
