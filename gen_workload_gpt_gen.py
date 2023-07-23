import os
import argparse
import math

parser = argparse.ArgumentParser()
parser.add_argument("-s", default=1, help="start sequence number")
parser.add_argument("-e", default=64, help="end sequence number")
parser.add_argument("-l", default="all", help="layers to generate - all: all layers, QKV: Q x K_T x V, noQKV: all except QKV")

def gen_actual_workload(folder, name, M_tile, al, m, k, n, mc):
    fout = open(folder + '/' + name, 'w')
    fout.write(", ".join(['1', '1', str(M_tile), str(al), str(mc)]) + '\n')
    fout.write(", ".join([str(m), str(k), str(n)]) + '\n')
    fout.close()

def gen_workload(n_heads, d_head, start, end, layer, model):
    d_model = n_heads * d_head
    M_tile = 2048
    al = 8

    folder = "workloads/" + model + "/GEN"
    if not os.path.exists(folder):
        os.makedirs(folder)


    if layer == "all" or "noQKV":
        # Create Q, K, V : 1 x d_model x d_model
        gen_actual_workload(folder, "createQKV", M_tile, al, d_model, d_model, 1, 16)
        # FC : 1 x d_model x d_model
        # no need

        # Linear 1 : 1 x d_model x 4*d_model
        gen_actual_workload(folder, "L1", M_tile, al, 4*d_model, d_model, 1, 16)

        # Linear 2 : 1 x 4*d_model x d_model
        gen_actual_workload(folder, "L2", M_tile, al, d_model, 4*d_model, 1, 16)
    if layer != "noQKV":
        for i in range(start, end+1):
            # Q x K_T : 1 x d_head x seq_num
            mc = 16
            if i <= 16:
                m = max(2, i)
                mc = 1
            else:
                m = i

            gen_actual_workload(folder, '_'.join(["QK", format(i, '04')]), M_tile, al, m, d_head, 1, mc)

            # S x V : 1 x seq_num x d_head
            gen_actual_workload(folder, '_'.join(["SV", format(i, '04')]), M_tile, al, d_head, i, 1, mc)

    return


if __name__ == "__main__":
    args = parser.parse_args()
    fin = open('models', 'r')
    lines = fin.readlines()
    for line in lines:
        line = line.strip()
        model, n_heads, d_head, _ = line.split(' ')
        gen_workload(int(n_heads), int(d_head), int(args.s), int(args.e), args.l, model)
