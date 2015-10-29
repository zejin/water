#!/bin/sh

module load maqao

#amplxe-cl -report hotspots -r r000ah/ > amplxe.txt
maqao cqa ./shallow fct=compute_step uarch=HASWELL > maqao.txt
