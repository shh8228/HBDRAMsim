import os
import argparse
import math

parser = argparse.ArgumentParser()
parser.add_argument("-s", default=1, help="start sequence number")
parser.add_argument("-e", default=64, help="end sequence number")
parser.add_argument("-l", default="all", help="layers to generate - all: all layers, QKV: Q x K_T x V, noQKV: all except QKV")

def gen_actual_workload(folder, name, M_tile, al, m, k, n, mcf, ucf):
    fout = open(folder + '/' + name, 'w')
    fout.write(", ".join(['1', '1', str(M_tile), str(al), str(mcf), str(ucf), '1']) + '\n')
    fout.write(", ".join([str(m), str(k), str(n)]) + '\n')
    fout.close()

def gen_workload(n_heads, d_head, d_model, start, end, layer, model, mcf, p):
    M_tile = 2048
    al = 8

    folder = "workloads/" + model + "/GEN_mcf" + str(mcf)
    if not os.path.exists(folder):
        os.makedirs(folder)


    if layer == "all" or "noQKV":
        # Create Q, K, V : 1 x d_model x d_head*n_heads/p
        gen_actual_workload(folder, "createQKV", M_tile, al, d_head*n_heads/p, d_model, 1, mcf, 16/mcf)
        # W_o : 1 x d_head*n_heads/p x d_model
        gen_actual_workload(folder, "W_o", M_tile, al, d_model, d_head*n_heads/p, 1, mcf, 16/mcf)

        # Linear 1 : 1 x d_model x 4*d_model/p
        gen_actual_workload(folder, "L1", M_tile, al, 4*d_model/p, d_model, 1, mcf, 16/mcf)

        # Linear 2 : 1 x 4*d_model/p x d_model
        gen_actual_workload(folder, "L2", M_tile, al, d_model, 4*d_model/p, 1, mcf, 16/mcf)
    if layer != "noQKV":
        for i in range(start, end+1):
            # Q x K_T : 1 x d_head x seq_num
            if i <= 16:
                m = max(2, i)
                mcf_ = 1
            else:
                m = i
                mcf_ = mcf

            assert d_head <= 128
            gen_actual_workload(folder, '_'.join(["QK", format(i, '04')]), M_tile, al, m, d_head, 1, mcf_, 1)

            # S x V : 1 x seq_num x d_head
            gen_actual_workload(folder, '_'.join(["SV", format(i, '04')]), M_tile, al, d_head, i, 1, mcf_, 1)

    return


if __name__ == "__main__":
    args = parser.parse_args()
    fin = open('models_s', 'r')
    lines = fin.readlines()
    for line in lines:
        line = line.strip()
        model, n_layers, d_model, n_heads, d_head, par = line.split(' ')
        for i in [1, 2, 4, 8, 16]:
            gen_workload(int(n_heads), int(d_head), int(d_model), int(args.s), int(args.e), args.l, model, i, int(par))
