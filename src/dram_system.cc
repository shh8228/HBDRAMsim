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
            //std::cout<<clk_ << "\tWait Refresh\n";
        }
    }

    //TODO npu signals
    bool weight_fetching = false;
    bool input_sending = false;
    int tensor_bitwidth = 3;
    bool input_trans_found = false;
    bool weight_trans_found = false;
    bool output_trans_found = false;



    if (!pim_trans_queue_.empty()) { // wait_refresh: when sending an activation cmd. no configured var
        std::cout<<clk_<<"\tTransaction Queue size: " << pim_trans_queue_.size() << '\n';
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

        auto it = pim_trans_queue_.begin();
        uint64_t address = it->addr;

        if (it->addr & 1) { // computing
            address = address >> 1;
            int cuts = vcuts * hcuts;
            bool configured = true;
            for (int i=0; i<cuts; i++)
                if ((address & (1 << i)) && (M[i] != 0 && N[i] != 0 && K[i] != 0));
                else configured = false;
            if (configured) {
                for (int i=0; i<cuts; i++)
                    if(address & (1 << i))
                        in_pim[i] = true;
                pim_trans_queue_.erase(it);
            }
        }
        else if ((it->addr & (1 << 6)) && (it->addr & (1 << 5))) { // cutting


            base_rows_w.clear();
            base_rows_in.clear();
            base_rows_out.clear();
            M.clear();
            N.clear();
            K.clear();
            M_it.clear();
            K_tile_it.clear();
            N_it.clear();
            M_out_it.clear();
            N_out_tile_it.clear();
            in_pim.clear();
            iw_status.clear();
            in_cnt.clear();
            out_cnt.clear();
            output_ready.clear();

            address = address >> 1 >> 4 >> 2; // trans_type, cut_no, loadType
            vcuts = 1 << (address & ((1<<bw_vcuts)-1));
            address = address >> bw_vcuts;
            hcuts = 1 << (address & ((1<<bw_hcuts)-1));
            address = address >> bw_hcuts;
            M_tile_size = 1 << (address & ((1<<bw_Mtile)-1));
            address = address >> bw_Mtile;
            vcuts_next = 1 << (address & ((1<<bw_vcuts)-1));
            address = address >> bw_vcuts;
            hcuts_next = 1 << (address & ((1<<bw_hcuts)-1));
            address = address >> bw_hcuts;
            kernel_size = address & ((1<<bw_kernelSize)-1);
            address = address >> bw_kernelSize;
            stride = address & ((1<<bw_stride)-1);

            assert(M_tile_size < 2048); // TODO accurate value

            int cuts = vcuts * hcuts;
            base_rows_w.assign(cuts, 0);
            base_rows_in.assign(cuts, 0);
            base_rows_out.assign(cuts, 0);
            M.assign(cuts, 0);
            N.assign(cuts, 0);
            K.assign(cuts, 0);
            M_it.assign(cuts, 0);
            K_tile_it.assign(cuts, 0);
            N_it.assign(cuts, 0);
            M_out_it.assign(cuts, 0);
            N_out_tile_it.assign(cuts, 0);
            in_pim.assign(cuts, false);
            iw_status.assign(cuts, 0);
            in_cnt.assign(cuts, -1);
            out_cnt.assign(cuts, -1);
            output_ready.assign(cuts, 0);

            pim_trans_queue_.erase(it);
        }
        else { // loading
            address = address >> 1;
            cut_no = address & ((1 << bw_cutNo)-1);
            address = address >> 4;
            int loadType = address & ((1<<bw_loadType)-1);
            address = address >> bw_loadType;
            int dim_value = address & (((uint64_t)1<<bw_dimValue)-1);
            address = address >> bw_dimValue;
            uint64_t base_row = address & ((1<<bw_baseRow)-1);
            address = address >> bw_baseRow;
            switch(loadType) {
                case 0: // M, weight
                    base_rows_w[cut_no] = base_row;
                    M[cut_no] = dim_value;
                    break;
                case 1: // K, output
                    base_rows_out[cut_no] = base_row;
                    std::cout<<base_row<<std::endl;
                    K[cut_no] = dim_value;
                    break;
                case 2: // N, input
                    base_rows_in[cut_no] = base_row;
                    N[cut_no] = dim_value;
                    break;
                default:
                    std::cerr << "Invalid load type!"
                              << std::endl;
                    AbruptExit(__FILE__, __LINE__);
                    break;
            }
            pim_trans_queue_.erase(it);

        }
    }

    int cuts = 0;
    if (vcuts != -1 && hcuts != -1) cuts = vcuts * hcuts;
    for (int i=0; i < cuts; i++) {
        if (!in_pim[i]) continue;

        int vcut_no = i % vcuts;
        int cut_height = config_.channels / hcuts;
        int hcut_no = i / vcuts;
        int cut_width = config_.banks / vcuts;

        int N_tile_size = 128 / vcuts; // TODO 128 : the number of PEs in a row
        int N_tile_it = N_it[i] / N_tile_size;
        int M_tile_it = M_it[i] / M_tile_size;
        int M_current_tile_size = M[i] < M_tile_size * (M_tile_it + 1) ? M[i] % M_tile_size : M_tile_size;
        int K_tile_size = std::min(cut_height * 16, K[i]); // TODO 16: the number of PEs supported by a bank's io


        // TODO
        int weight_banks_reduce = 2;
        std::vector<Command> cmds;

        switch (iw_status[i]) {
            case 0: { // Fetching weight
                int N_tile_size_per_bank = N_tile_size/(cut_width/weight_banks_reduce);
                int col_offset = N_tile_it * (N_tile_size_per_bank * ((K[i]-1) / K_tile_size + 1)) + K_tile_it[i] * N_tile_size_per_bank + N_it[i] % N_tile_size; // N_it incremented by N_tile_size when N_it % N_tile_size_per_bank == 0 (but not with N_tile_size)
                for (int j=0; j<cut_height; j++) {
                   for (int k=0; k<cut_width/weight_banks_reduce; k++) {
                        // TODO offset must be divided by row_width/dev_width
                        Address addr = Address(hcut_no * cut_height + j, 0, 0, vcut_no * cut_width + k * weight_banks_reduce, base_rows_w[i] + col_offset/32,  col_offset % 32);
                        uint64_t hex_addr = config_.AddressUnmapping(addr);
                        Command cmd = Command(CommandType::READ, addr, hex_addr);
                        cmds.push_back(cmd);
                    }
                }

                std::cout<<"Fetch Weight\n";

                // check issuability
                bool issuable = true;

                // issue


                // increment iterators
                N_it[i]++;
                if (N_it[i] % N_tile_size_per_bank == 0 && N_it[i] % N_tile_size != 0) {
                    N_it[i] = N_tile_size * N_tile_it;
                    iw_status[i]++;

                }



                break;
            }
            case 1: { // Finished weight
                // wait npu_signals
                iw_status[i]++;

                if ((K_tile_it[i]+1) * K_tile_size >= K[i])
                    out_cnt[i] = 3 + 16; // TODO log(8) + 128 / 8
                break;
            }
            case 2: { // Feeding input

                int col_offset = M_tile_it * (M_tile_size * ((K[i]-1) / K_tile_size + 1)) + K_tile_it[i] * M_current_tile_size + M_it[i] % M_tile_size;
                for (int j=0; j<cut_height; j++) {
                    // TODO offset must be divided by row_width/dev_width
                    Address addr = Address(hcut_no * cut_height + j, 0, 0, vcut_no * cut_width, base_rows_in[i] + col_offset/32,  col_offset % 32);
                    uint64_t hex_addr = config_.AddressUnmapping(addr);
                    Command cmd = Command(CommandType::READ, addr, hex_addr);
                    cmds.push_back(cmd);
                }

                std::cout<<"Feed Input\n";

                // check issuability
                bool issuable = true;

                M_it[i]++;
                if (M_it[i] % M_tile_size == 0 || M_it[i] == M[i]) {

                    in_cnt[i] = std::max(1, 128/vcuts - 30); // TODO subtract tRP+tRCD
                    iw_status[i]++;

                    M_it[i] = M_tile_size * M_tile_it;
                    K_tile_it[i]++;

                    if (K_tile_it[i] * K_tile_size >= K[i]) {
                        K_tile_it[i] = 0;
                        N_it[i] = N_tile_size * (N_tile_it+1);
                        if (N_it[i] >= N[i]) {
                            N_it[i] = 0;
                            M_it[i] = M_tile_size * (M_tile_it+1);
                            if (M_it[i] >= M[i]) {
                                std::cout<<"End of Computation\n";
                                in_cnt[i] = -1;
                            }
                        }
                    }
                }
                break;
            }
            case 3: {// Finished input
                if (in_cnt[i] != -1) {
                    in_cnt[i]--;
                    if (in_cnt[i] == 0)
                        iw_status[i] = 0;
                }
                break;
            }
            default: {
                break;
            }
        }

        // TODO simulate npu_signal


        if (out_cnt[i] == 0) output_ready[i]++;
        if (out_cnt[i] != -1) out_cnt[i]--;



        if (output_ready[i] > 0) {
            int vcut_out_no = (vcut_no + N_out_tile_it[i]) % vcuts; // relates to channel number
            int M_out_tile_it = M_out_it[i] / M_tile_size;
            int M_out_current_tile_size = M[i] < M_tile_size * (M_out_tile_it + 1) ? M[i] % M_tile_size : M_tile_size;
            int N_tile_num = (N[i]-1) / N_tile_size + 1;
            int N_tile_num_ch = (N_tile_num) / vcuts; // varies by channels to be accessed
            N_tile_num_ch += N_tile_num % vcuts > N_out_tile_it[i] % vcuts ? 1 : 0;
            int N_tile_it_ch = N_out_tile_it[i] / vcuts;
            int col_offset = M_out_tile_it * (M_tile_size * N_tile_num_ch) + N_tile_it_ch * M_out_current_tile_size + M_out_it[i] % M_tile_size;

            for (int j=0; j<cut_height / vcuts; j++) {
                // TODO offset must be divided by row_width/dev_width
                Address addr = Address(hcut_no * cut_height + vcut_out_no * (cut_height / vcuts) + j, 0, 0, vcut_no * cut_width + 1, base_rows_out[i] + col_offset/32,  col_offset % 32);
                uint64_t hex_addr = config_.AddressUnmapping(addr);
                Command cmd = Command(CommandType::WRITE, addr, hex_addr);
                cmds.push_back(cmd);
            }

            std::cout<<"Write Output\n";

            M_out_it[i]++;
            if (M_out_it[i] % M_tile_size == 0 || M_out_it[i] == M[i]) {
                M_out_it[i] = M_tile_size * M_out_tile_it;
                N_out_tile_it[i]++;
                if (N_out_tile_it[i] * N_tile_size >= N[i]) {
                    N_out_tile_it[i] = 0;
                    M_out_it[i] = M_tile_size * (M_out_tile_it+1);
                    if (M_out_it[i] >= M[i]) {
                        assert(in_cnt[i] == -1);
                        std::cout<<"Output Exhausted. Turn off PIM mode.\n";
                        in_pim[i] = false;
                    }

                }

                output_ready[i]--;
                // Output Tile Finished
            }
        }
        for (auto& it: cmds) {
            std::cout<<it<<std::endl;
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
