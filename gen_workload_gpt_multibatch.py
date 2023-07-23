import os
import argparse
import math

parser = argparse.ArgumentParser()
parser.add_argument("-b", default=1, help="batch size")

def gen_actual_workload_ws_mc(folder, name, M_tile, al, m, k, n, mc):
    if n < mc:
        return
    fout = open(folder + '/' + name, 'w')
    fout.write(", ".join(['1', '1', str(M_tile), str(al), str(mc)]) + '\n')
    fout.write(", ".join([str(m), str(k), str(n)]) + '\n')
    fout.close()

def gen_workload(n_heads, d_head, batch_size, model):
    d_model = n_heads * d_head
    M_tile = 2048
    al = 8

    for mc in [1, 2, 4, 8]:
        folder = "workloads/" + model + "/multibatch_GEN/" +  "mc" + str(mc) + '_' + format(batch_size, '04')
        if not os.path.exists(folder):
            os.makedirs(folder)


        # Create Q, K, V : batch_size x d_model x d_model
        gen_actual_workload_ws_mc(folder, "createQKV", M_tile, al, batch_size, d_model, d_model, mc)

        # FC : batch_size x d_model x d_model
        # no need

        # Linear 1 : batch_size x d_model x 4*d_model
        gen_actual_workload_ws_mc(folder, "L1", M_tile, al, batch_size, d_model, 4*d_model, mc)

        # Linear 2 : batch_size x 4*d_model x d_model
        gen_actual_workload_ws_mc(folder, "L2", M_tile, al, batch_size, 4*d_model, d_model, mc)


    return


if __name__ == "__main__":
    args = parser.parse_args()
    fin = open('models', 'r')
    lines = fin.readlines()
    for line in lines:
        line = line.strip()
        model, n_heads, d_head, _ = line.split(' ')
        gen_workload(int(n_heads), int(d_head), int(args.b), model)
