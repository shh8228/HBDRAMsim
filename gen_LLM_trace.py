import os
import argparse
import math

parser = argparse.ArgumentParser()
parser.add_argument("-w", default=".", help="workloads file")
parser.add_argument("-t", default="./sample.trc", help="trace file")
parser.add_argument("-f", default="True", help="given folders")

import configparser

def extract_variables(file_path):
    desired_variables = {
        "dram_structure": ["protocol", "bankgroups", "banks_per_group", "rows", "columns", "device_width", "BL", "num_dies"],
        "timing": ["tCK", "CL", "CWL", "tRCDRD", "tRCDWR", "tRP", "tRAS", "tRFC", "tREFI"],
        "power": ["VDD", "IDD0", "IDD2P", "IDD2N", "IDD3P", "IDD3N", "IDD4W", "IDD4R", "IDD5AB", "IDD6x"],
        "system": ["channel_size", "channels", "bus_width", "address_mapping", "queue_structure", "row_buf_policy", "cmd_queue_size", "trans_queue_size", "unified_queue"]
    }

    config = configparser.ConfigParser()
    config.read(file_path)

    extracted_data = {}

    for section, variables in desired_variables.items():
        if section in config:
            extracted_data[section] = {}
            for var in variables:
                if var in config[section]:
                        extracted_data[section][var] = config[section][var]

    return extracted_data

file_path = "path/to/your/file.ini"

extracted_data = extract_variables(file_path)
for section, variables in extracted_data.items():
        print(f"[{section}]")
            for var, value in variables.items():
                        print(f"{var} = {value}")
                            print()

def gen_pim_trace(workload, trace_file):
    fin = open(workload, 'r')
    fout = open(trace_file, 'w')
    line = fin.readline()
    cutV, cutH, tile_M, post_delay, mcf, ucf, df = eval(line)
    # print(cutV, cutH, tile_M, post_delay)

    exp = 5

    cutaddr = 3 * (2**exp)
    exp += 2
    cutaddr += math.log(cutV, 2) * (2**exp)
    exp += 3
    cutaddr += math.log(cutH, 2) * (2**exp)
    exp += 1
    cutaddr += math.log(mcf, 2) * (2**exp)
    exp += 3
    cutaddr += math.log(ucf, 2) * (2**exp)
    exp += 3

    cutaddr += df * (2**exp)
    exp += 1

    cutaddr += math.log(tile_M, 2) * (2**exp)

    loadaddrs = []

    cutSize = cutV*cutH
    for i in range(cutV*cutH):
        line = fin.readline()
        dims = eval(line)
        # print(dims)
        for (j, dim) in enumerate(dims):
            # print(j, dim)
            exp = 1
            loadaddr = i * (2**exp)
            exp += 4
            loadaddr += j * (2**exp)
            exp += 2
            if (dims[2] != 1 and j==0):
                dim = int(dim/2) # GEMM interleaving
            if (dims[2] == 1): # N[i] == 1 (mcf*ucf == 16)
                if (j == 0):
                    loadaddr += max(1, dim/mcf) * (2**exp)
                elif (j == 1):
                    loadaddr += max(1, dim/ucf) * (2**exp)
                else:
                    loadaddr += dim*ucf*8 * (2**exp) # TODO *8 to single batch is nono, if df == 1, write just dim*ucf
            else:
                loadaddr += dim * (2**exp)
            exp += 32
            if (dims[2] != 1 and j!=0): # Allocating addresses of Inputs and outputs in GEMM kernel
                loadaddr += (int(dims[1]*dims[2]/1024)+1) * (2**exp)
            elif (dims[2] == 1 and j!=2): # Allocating addresses of Inputs and outputs in GEMV kernel
                loadaddr += (int(dims[0]*dims[1]/1024)+1) * (2**exp)
            else: # Allocating address of Weights
                loadaddr += 0
            loadaddrs.append(loadaddr)
    exp = 0
    compaddr = 1
    exp += 1
    compaddr += (2**(cutV*cutH)-1) * (2**exp)

    fin.close()

    cycle = 0
    fout.write(hex(int(cutaddr)) + '\tPIM\t' + str(cycle) + '\n')
    cycle += 1
    for addr in loadaddrs:
        fout.write(hex(int(addr)) + '\tPIM\t' + str(cycle) + '\n')
        cycle += 1
    fout.write(hex(int(compaddr)) + '\tPIM\t' + str(cycle) + '\n')

    fout.close()
    return


if __name__ == "__main__":
    args = parser.parse_args()
    if eval(args.f):
        fin_ = open('models_s', 'r')
        lines = fin_.readlines()
        for line in lines:
            line = line.strip()
            model = line.split(' ')[0]
            print(model)
            for t in ["/SUM/"] + ["/GEN_mcf" + str(i) + '/' for i in [1, 2, 4, 8, 16]]:
                workloads_path = "workloads/" + model + t
                trace_path = "traces/" + model + t
                flist = os.listdir(workloads_path)
                for f in flist:
                    if os.path.isdir(workloads_path + f):
                        if not os.path.exists(trace_path + f):
                            os.makedirs(trace_path + f)
                        fflist = os.listdir(workloads_path + f)
                        for ff in fflist:
                            gen_pim_trace(workloads_path + f + '/' + ff, trace_path + f + '/' + ff)
                    else:
                        if not os.path.exists(trace_path):
                            os.makedirs(trace_path)
                        gen_pim_trace(workloads_path + f, trace_path + f)

    else:
        gen_pim_trace(args.w, args.t)
