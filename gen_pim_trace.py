import argparse
import math

parser = argparse.ArgumentParser()
parser.add_argument("-w", default="sample.workload", help="workload file")
parser.add_argument("-t", default="sample.trc", help="trace file")

rowSize = 32
bankX, bankY = 16, 8
peX, peY = 128, 128
w_reads_per_tile = peX / (bankX/2) # how many reads we need per tile?

def gen_pim_trace(workload, trace_file):
    fin = open(workload, 'r')
    fout = open(trace_file, 'w')
    line = fin.readline()
    cutV, cutH, tile_M, post_delay = eval(line)
    print(cutV, cutH, tile_M, post_delay)
    cutSize = cutV*cutH
    dims = []
    for i in range(cutV*cutH):
        line = fin.readline()
        dims.append((eval(line)))
    print(dims)
    fin.close()

    rows = []
    cycle = 10
    for (M, K, N) in dims:
        #TODO weight fetch method?
        # assume M,N,K is power of 2
        num_tiles_M = M/tile_M + M%tile_M
        num_tiles_N = N/peY + N%peY
        num_tiles_K = K/peX + K%peX
        for i in range(num_tiles_M):
            # next batch input
            for j in range(num_tiles_N):
                # output write
                # input go back and be reused
                for k in range(num_tiles_K):
                    if w_reads_per_tile < rowSize:
                        w_tiles_per_row = rowSize / w_reads_per_tile # how many tiles available for each row?
                        w_ofs = k % w_tiles_per_row
                        # make one transaction
                        w_rows = [k / w_tiles_per_row + j * ((num_tiles_K)/w_tiles_per_row)]
                    else:
                        rows_per_tile = w_reads_per_tile / rowSize # how many rows needed for each tile?
                        w_ofs = 0
                        # make w_rows_per_tile transactions
                        w_rows = [kk + k*rows_per_tile + j * ((num_tiles_K)*rows_per_tile) for kk in range(rows_per_tile)]
                    for w_row in w_rows:
                        addr = 1 + (2**3) * (math.log(cutV, 2)) + (2**5) * (2**cutV-1) + (2**13) * (math.log(cutH, 2)) + (2**14) * (2**cutH-1) + (2**16) * (w_ofs) + (2**17) * (w_row)
                        fout.write(hex(int(addr)) + '\tPIM\t' + str(cycle) + '\n')
                        cycle += 1


                    # input tile feed
                    for l in range(tile_M/rowSize):
                        in_row = l + k * (tile_M/rowSize) + i * (num_tiles_K) * (tile_M/rowSize)
                        addr = 0 + (2**3) * (math.log(cutV, 2)) + (2**5) * (2**cutV-1) + (2**13) * (math.log(cutH, 2)) + (2**14) * (2**cutH-1) + (2**17) * (in_row)
                        fout.write(hex(int(addr)) + '\tPIM\t' + str(cycle) + '\n')
                        cycle += 1
                for jj in range(tile_M/rowSize):
                    out_row = jj + j * (tile_M/rowSize) + i * (num_tiles_N) * (tile_M/rowSize)
                    addr = 2 + (2**3) * (math.log(cutV, 2)) + (2**5) * (2**cutV-1) + (2**13) * (math.log(cutH, 2)) + (2**14) * (2**cutH-1) + (2**17) * (out_row)
                    fout.write(hex(int(addr)) + '\tPIM\t' + str(cycle + post_delay + jj) + '\n')



        temp = M*K*N / rowSize
        total_in_rows = temp / N
        total_w_rows = temp / M
        total_out_rows = temp / K

        total_tiles = M / tile_M
        rows_per_tile = tile_M / rowSize
        rows.append(total_tiles*rows_per_tile)

    fout.close()
    return


if __name__ == "__main__":
    args = parser.parse_args()
    gen_pim_trace(args.w, args.t)
