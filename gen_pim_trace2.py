import os
import argparse
import math

parser = argparse.ArgumentParser()
parser.add_argument("-w", default=".", help="workloads folder")
parser.add_argument("-t", default=".", help="trace folder")
parser.add_argument("-f", default="True", help="given folders")

def gen_pim_trace(workload, trace_file):
    fin = open(workload, 'r')
    fout = open(trace_file, 'w')
    line = fin.readline()
    cutV, cutH, tile_M, post_delay, mc = eval(line)
    # print(cutV, cutH, tile_M, post_delay)

    exp = 5

    cutaddr = 3 * (2**exp)
    exp += 2
    cutaddr += math.log(cutV, 2) * (2**exp)
    exp += 3
    cutaddr += math.log(cutH, 2) * (2**exp)
    exp += 1
    cutaddr += math.log(mc, 2) * (2**exp)
    exp += 3
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
            loadaddr += dim * (2**exp)
            exp += 32
            if (j==0):
                loadaddr += (max((dims[0]*dims[2])/2048, (dims[0]*dims[1])/2048) + 1) * (2**exp)
            else:
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
        fin_ = open('models', 'r')
        lines = fin_.readlines()
        for line in lines:
            line = line.strip()
            model, n_heads, d_head, _ = line.split(' ')
            for t in ["/SUM/", "/multibatch_GEN/", "/GEN/"]:
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
