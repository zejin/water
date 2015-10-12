#!/bin/sh

cd ~/water-zejin

module load cs5220
module load maqao

amplxe-cl -report hotspots -r r001ah/ > amplxe.txt
maqao cqa ./shallow fct=compute_step uarch=HASWELL > maqao.txt