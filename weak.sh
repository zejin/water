#!/bin/sh

ncells=100

for n in {1..10}
do
nthreads=$((n*n))

file="weak_${nthreads}.pbs"
echo "#!/bin/sh -l" >> $file
echo "#PBS -l nodes=1:ppn=24" >> $file
echo "#PBS -l walltime=0:30:00" >> $file
echo "#PBS -N shallow" >> $file
echo "#PBS -j oe" >> $file
echo "module load cs5220" >> $file
echo "cd \$PBS_O_WORKDIR" >> $file

echo "echo 'weak scaling'">> $file
echo "echo 'number of threads: $nthreads'" >> $file
echo "echo 'number of cells: $((ncells*n))'" >> $file

echo "export OMP_NUM_THREADS=$nthreads" >> $file
echo "amplxe-cl -collect advanced-hotspots ./shallow -i dam_break -o dam_break.out -n $((ncells*n)) -t $nthreads -s weak" >> $file

qsub $file
rm $file
done