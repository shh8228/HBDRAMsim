import os
import argparse
import math

parser = argparse.ArgumentParser()
parser.add_argument("-s", default=1, help="sequence length")

def gen_actual_workload_ws_mc(folder, name, M_tile, al, m, k, n, mc):
    if n < mc:
        return
    fout = open(folder + '/' + name, 'w')
    fout.write(", ".join(['1', '1', str(M_tile), str(al), str(mc), str(1), '0']) + '\n')
    fout.write(", ".join([str(m), str(k), str(n)]) + '\n')
    fout.close()


def gen_workload(n_heads, d_head, seq_len, model):
    d_model = n_heads * d_head
    M_tile = 2048
    al = 8

    for mc in [1, 2, 4, 8]:
        folder = "workloads/" + model + "/SUM/" + format(seq_len, '04') + "_mc" + str(mc)
        if not os.path.exists(folder):
            os.makedirs(folder)


        # Create Q, K, V : seq_len x d_model x d_model
        gen_actual_workload_ws_mc(folder, "createQKV", M_tile, al, seq_len, d_model, d_model, mc)

        # FC : seq_len x d_model x d_model
        # no need

        # Linear 1 : seq_len x d_model x 4*d_model
        gen_actual_workload_ws_mc(folder, "L1", M_tile, al, seq_len, d_model, 4*d_model, mc)

        # Linear 2 : seq_len x 4*d_model x d_model
        gen_actual_workload_ws_mc(folder, "L2", M_tile, al, seq_len, 4*d_model, d_model, mc)

        # Q x K_T : seq_len x d_head x seq_len
        gen_actual_workload_ws_mc(folder, "QK", M_tile, al, seq_len, d_head, seq_len, mc)

        # S x V : seq_len x seq_len x d_head
        gen_actual_workload_ws_mc(folder, "SV", M_tile, al, seq_len, seq_len, d_head, mc)

    return


if __name__ == "__main__":
    args = parser.parse_args()
    fin = open('models', 'r')
    lines = fin.readlines()
    for line in lines:
        line = line.strip()
        model, n_heads, d_head, _ = line.split(' ')
        gen_workload(int(n_heads), int(d_head), int(args.s), model)
