#!/bin/sh

ncells=100

for nthreads in {1..10}
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
echo "amplxe-cl -collect advanced-hotspots ./shallow -i dam_break -o dam_break.out -n $ncells" >> $file

qsub $file
rm $file
done

for nthreads in {1..10}
do
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
echo "echo 'number of cells: $((ncells*nthreads))'" >> $file

echo "export OMP_NUM_THREADS=$nthreads" >> $file
echo "amplxe-cl -collect advanced-hotspots ./shallow -i dam_break -o dam_break.out -n $((ncells*nthreads))" >> $file

qsub $file
rm $file
done