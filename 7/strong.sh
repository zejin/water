#!/bin/sh

ncell=200

for nthread in {1..8}
do
file="strong_${nthread}.pbs"

echo "#!/bin/sh -l" >> $file
echo "#PBS -l nodes=1:ppn=24" >> $file
echo "#PBS -l walltime=0:30:00" >> $file
echo "#PBS -N shallow" >> $file
echo "#PBS -j oe" >> $file
echo "module load cs5220" >> $file
echo "cd \$PBS_O_WORKDIR" >> $file

echo "echo 'strong scaling'" >> $file
echo "echo 'number of threads: $((nthread*nthread))'" >> $file
echo "echo 'number of cells: $ncell'" >> $file

echo "export MKL_MIC_ENABLE=1" >> $file
#echo "export OMP_NUM_THREADS=16" >> $file
echo "export MIC_OMP_NUM_THREADS=240" >> $file

#echo "export OMP_NUM_THREADS=$nthreads" >> $file
echo "./lshallow tests.lua dam $ncell $((nthread*nthread)) 10 strong" >> $file

qsub $file
rm $file
done