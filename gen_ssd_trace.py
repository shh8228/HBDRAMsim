import argparse
import math

parser = argparse.ArgumentParser()
parser.add_argument("-op", default="WRITE", help="OP")
parser.add_argument("-size", default=256, help="SIZE")
parser.add_argument("-t", default="ssd.trc", help="trace file")


def gen_pim_trace(op, size, trace_file):
    fout = open(trace_file, 'w')
    dev_w = 8 # in bytes
    for i in range(size*(2**10)/dev_w):
        fout.write('\t'.join([hex(int(i*dev_w)), op, str(i)]) + '\n')





    fout.close()
    return


if __name__ == "__main__":
    args = parser.parse_args()
    gen_pim_trace(args.op, int(args.size), args.t)
