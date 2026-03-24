#!/usr/bin/env python3
"""
Gera um arquivo GFA sintético com IDs realistas (ctg_XXXXXXXX).
Uso: python3 gen_gfa.py <num_segmentos> <saida.gfa>
"""
import sys
import random
import string

def rand_seq(length):
    return ''.join(random.choices('ACGT', k=length))

def rand_id(i):
    # IDs no estilo de montadores reais: ctg_<8 hex chars>
    return f"ctg_{i:08x}"

def main():
    if len(sys.argv) < 3:
        print(f"Uso: {sys.argv[0]} <num_segmentos> <saida.gfa>")
        sys.exit(1)

    n_seg = int(sys.argv[1])
    out   = sys.argv[2]

    # ~3 arestas por nó em média (típico de grafos de montagem de genoma)
    n_links = n_seg * 3

    print(f"Gerando {n_seg} segmentos e {n_links} arestas -> {out}")

    with open(out, 'w') as f:
        f.write("H\tVN:Z:1.0\n")

        for i in range(n_seg):
            seg_id  = rand_id(i)
            seq_len = random.randint(100, 50000)
            # Sequência omitida (*) para gerar arquivos menores; realista para GFAs de
            # montadores modernos como hifiasm/verkko que emitem sequência separada.
            f.write(f"S\t{seg_id}\t*\tLN:i:{seq_len}\n")

        orients = ['+', '-']
        for _ in range(n_links):
            a = random.randint(0, n_seg - 1)
            b = random.randint(0, n_seg - 1)
            oa = random.choice(orients)
            ob = random.choice(orients)
            f.write(f"L\t{rand_id(a)}\t{oa}\t{rand_id(b)}\t{ob}\t0M\n")

    print("Pronto.")

if __name__ == "__main__":
    main()
