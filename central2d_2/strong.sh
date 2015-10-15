#!/bin/sh

ncells=200

for nthreads in {1..9}
do
file="strong_${nthreads}.pbs"

echo "#!/bin/sh -l" >> $file
echo "#PBS -l nodes=1:ppn=24" >> $file
echo "#PBS -l walltime=0:30:00" >> $file
echo "#PBS -N shallow" >> $file
echo "#PBS -j oe" >> $file
echo "module load cs5220" >> $file
echo "cd \$PBS_O_WORKDIR" >> $file

echo "echo 'strong scaling'">> $file
echo "echo 'number of threads: $nthreads'" >> $file
echo "echo 'number of cells: $ncells'" >> $file

echo "export OMP_NUM_THREADS=$nthreads" >> $file
echo "./shallow -i dam_break -o dam_break.out -n $ncells -t $nthreads -s strong" >> $file

qsub $file
rm $file
done