import argparse
import math

parser = argparse.ArgumentParser()
parser.add_argument("-w", default="sample.workload", help="workload file")
parser.add_argument("-t", default="sample.trc", help="trace file")


def gen_pim_trace(workload, trace_file):
    fin = open(workload, 'r')
    fout = open(trace_file, 'w')
    line = fin.readline()
    cutV, cutH, tile_M, post_delay = eval(line)
    print(cutV, cutH, tile_M, post_delay)

    exp = 5

    cutaddr = 3 * (2**exp)
    exp += 2
    cutaddr += math.log(cutV, 2) * (2**exp)
    exp += 2
    cutaddr += math.log(cutH, 2) * (2**exp)
    exp += 1
    cutaddr += math.log(tile_M, 2) * (2**exp)

    loadaddrs = []

    cutSize = cutV*cutH
    for i in range(cutV*cutH):
        line = fin.readline()
        dims = eval(line)
        print(dims)
        for (j, dim) in enumerate(dims):
            print(j, dim)
            exp = 1
            loadaddr = i * (2**exp)
            exp += 4
            loadaddr += j * (2**exp)
            exp += 2
            loadaddr += dim * (2**exp)
            exp += 32
            if (j==0):
                loadaddr += ((dims[0]*dims[1])/512) * (2**exp)
            else:
                loadaddr += 0
            loadaddrs.append(loadaddr)
    exp = 0
    compaddr = 1
    exp += 1
    compaddr += (2**8-1) * (2**exp)

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
    gen_pim_trace(args.w, args.t)
