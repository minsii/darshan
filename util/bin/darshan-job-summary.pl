#!/usr/bin/perl -w
#
#  (C) 2009 by Argonne National Laboratory.
#      See COPYRIGHT in top-level directory.
#

use FindBin;
use lib "$FindBin::Bin/../lib/";
use TeX::Encode;
use Encode;
use File::Temp qw/ tempdir /;
use Cwd;
use Getopt::Long;
use English;
use Number::Bytes::Human qw(format_bytes);

my $gnuplot = "";

my $tmp_dir = tempdir( CLEANUP => 1 );
#my $tmp_dir = tempdir( CLEANUP => 0 );
#print "tmp dir: $tmp_dir\n";

my $orig_dir = getcwd;
my $output_file = "summary.pdf";
my $input_file = "";
my %access_hash = ();
my @access_size = ();
my %hash_files = ();

process_args();

# find a darshan-parser executable somewhere
my $darshan_parser = "";
if(-x "$FindBin::Bin/../bin/darshan-parser")
{
    $darshan_parser = "$FindBin::Bin/../bin/darshan-parser";
}
else
{
    $darshan_parser = "darshan-parser";
}

open(TRACE, "$darshan_parser $input_file |") || die("Can't execute \"$darshan_parser $input_file\": $!\n");

open(FA_READ, ">$tmp_dir/file-access-read.dat") || die("error opening output file: $!\n");
open(FA_WRITE, ">$tmp_dir/file-access-write.dat") || die("error opening output file: $!\n");
open(FA_READ_SH, ">$tmp_dir/file-access-read-sh.dat") || die("error opening output file: $!\n");
open(FA_WRITE_SH, ">$tmp_dir/file-access-write-sh.dat") || die("error opening output file: $!\n");

my $last_read_start = 0;
my $last_write_start = 0;

my $cumul_read_indep = 0;
my $cumul_read_bytes_indep = 0;

my $cumul_write_indep = 0;
my $cumul_write_bytes_indep = 0;

my $cumul_read_shared = 0;
my $cumul_read_bytes_shared = 0;

my $cumul_write_shared = 0;
my $cumul_write_bytes_shared = 0;

my $cumul_meta_shared = 0;
my $cumul_meta_indep = 0;

my $first_data_line = 1;
my $current_rank = 0;
my $current_hash = 0;
my %file_record_hash = ();

my %fs_data = ();

while ($line = <TRACE>) {
    chop($line);
    
    if ($line =~ /^\s*$/) {
        # ignore blank lines
    }
    elsif ($line =~ /^#/) {
	if ($line =~ /^# exe: /) {
	    ($junk, $cmdline) = split(':', $line, 2);
            # add escape characters if needed for special characters in
            # command line
            $cmdline = encode('latex', $cmdline);
	}
	if ($line =~ /^# nprocs: /) {
	    ($junk, $nprocs) = split(':', $line, 2);
	    $procreads[$nprocs] = 0;
	}
	if ($line =~ /^# run time: /) {
	    ($junk, $runtime) = split(':', $line, 2);
	}
	if ($line =~ /^# start_time: /) {
	    ($junk, $starttime) = split(':', $line, 2);
	}
	if ($line =~ /^# uid: /) {
	    ($junk, $uid) = split(':', $line, 2);
	}
        if ($line =~ /^# jobid: /) {
	    ($junk, $jobid) = split(':', $line, 2);
        }
    }
    else {
        # parse line
	@fields = split(/[\t ]+/, $line);

        # is this our first piece of data?
        if($first_data_line)
        {
            $current_rank = $fields[0];
            $current_hash = $fields[1];
            $first_data_line = 0;
        }

        # is this a new file record?
        if($fields[0] != $current_rank || $fields[1] != $current_hash)
        {
            $file_record_hash{CP_NAME_SUFFIX} = $fields[4];

            # process previous record
            process_file_record($current_rank, $current_hash, \%file_record_hash);

            # reset variables for next record 
            $current_rank = $fields[0];
            $current_hash = $fields[1];
            %file_record_hash = ();
        }

        $file_record_hash{$fields[2]} = $fields[3];

	$summary{$fields[2]} += $fields[3];

	# record per-process POSIX read count
	if ($fields[2] eq "CP_POSIX_READS") {
	    if ($fields[0] == -1) {
		$procreads[$nprocs] += $fields[3];
	    }
	    else {
		$procreads[$fields[0]] += $fields[3];
	    }
	}

	# record per-proces POSIX write count
	if ($fields[2] eq "CP_POSIX_WRITES") {
	    if ($fields[0] == -1) {
		$procwrites[$nprocs] += $fields[3];
	    }
	    else {
		$procwrites[$fields[0]] += $fields[3];
	    }
	}

        # seperate accumulators for independent and shared reads and writes
        if ($fields[2] eq "CP_F_POSIX_READ_TIME" && $fields[0] == -1){
            $cumul_read_shared += $fields[3];
        }
        if ($fields[2] eq "CP_F_POSIX_READ_TIME" && $fields[0] != -1){
            $cumul_read_indep += $fields[3];
        }
        if ($fields[2] eq "CP_F_POSIX_WRITE_TIME" && $fields[0] == -1){
            $cumul_write_shared += $fields[3];
        }
        if ($fields[2] eq "CP_F_POSIX_WRITE_TIME" && $fields[0] != -1){
            $cumul_write_indep += $fields[3];
        }

        if ($fields[2] eq "CP_F_POSIX_META_TIME" && $fields[0] == -1){
            $cumul_meta_shared += $fields[3];
        }
        if ($fields[2] eq "CP_F_POSIX_META_TIME" && $fields[0] != -1){
            $cumul_meta_indep += $fields[3];
        }

        if ((($fields[2] eq "CP_BYTES_READ") or
             ($fields[2] eq "CP_BYTES_WRITTEN")) and
            not defined($fs_data{$fields[5]}))
        {
            $fs_data{$fields[5]} = [0,0];
        }

        if ($fields[2] eq "CP_BYTES_READ" && $fields[0] == -1){
            $cumul_read_bytes_shared += $fields[3];
            $fs_data{$fields[5]}->[0] += $fields[3];
        }
        if ($fields[2] eq "CP_BYTES_READ" && $fields[0] != -1){
            $cumul_read_bytes_indep += $fields[3];
            $fs_data{$fields[5]}->[0] += $fields[3];
        }
        if ($fields[2] eq "CP_BYTES_WRITTEN" && $fields[0] == -1){
            $cumul_write_bytes_shared += $fields[3];
            $fs_data{$fields[5]}->[1] += $fields[3];
        }
        if ($fields[2] eq "CP_BYTES_WRITTEN" && $fields[0] != -1){
            $cumul_write_bytes_indep += $fields[3];
            $fs_data{$fields[5]}->[1] += $fields[3];
        }

        # record start and end of reads and writes

        if ($fields[2] eq "CP_F_READ_START_TIMESTAMP") {
            # store until we find the end
            # adjust for systems that give absolute time stamps
            $last_read_start = $fields[3];
        }
        if ($fields[2] eq "CP_F_READ_END_TIMESTAMP" && $fields[3] != 0) {
            # assume we got the read start already 
            my $xdelta = $fields[3] - $last_read_start;
            # adjust for systems that have absolute time stamps 
            if($last_read_start > $starttime) {
                $last_read_start -= $starttime;
            }
            if($fields[0] == -1){
                print FA_READ_SH "$last_read_start\t0\t$xdelta\t0\n";
            }
            else{
                print FA_READ "$last_read_start\t$fields[0]\t$xdelta\t0\n";
            }
        }
        if ($fields[2] eq "CP_F_WRITE_START_TIMESTAMP") {
            # store until we find the end
            $last_write_start = $fields[3];
        }
        if ($fields[2] eq "CP_F_WRITE_END_TIMESTAMP" && $fields[3] != 0) {
            # assume we got the write start already 
            my $xdelta = $fields[3] - $last_write_start;
            # adjust for systems that have absolute time stamps 
            if($last_write_start > $starttime) {
                $last_write_start -= $starttime;
            }
            if($fields[0] == -1){
                print FA_WRITE_SH "$last_write_start\t0\t$xdelta\t0\n";
            }
            else{
                print FA_WRITE "$last_write_start\t$fields[0]\t$xdelta\t0\n";
            }
        }

        if ($fields[2] =~ /^CP_ACCESS(.)_ACCESS/) {
            $access_size[$1] = $fields[3];
        }
        if ($fields[2] =~ /^CP_ACCESS(.)_COUNT/) {
            my $tmp_access_size = $access_size[$1];
            if(defined $access_hash{$tmp_access_size}){
                $access_hash{$tmp_access_size} += $fields[3];
            }
            else{
                $access_hash{$tmp_access_size} = $fields[3];
            }
        }
    }
}

# process last file record
$file_record_hash{CP_NAME_SUFFIX} = $fields[4];
process_file_record($current_rank, $current_hash, \%file_record_hash);
close(TRACE) || die "darshan-parser failure: $! $?";

# Fudge one point at the end to make xrange match in read and write plots.
# For some reason I can't get the xrange command to work.  -Phil
print FA_READ "$runtime\t-1\t0\t0\n";
print FA_WRITE "$runtime\t-1\t0\t0\n";
print FA_READ_SH "$runtime\t0\t0\t0\n";
print FA_WRITE_SH "$runtime\t0\t0\t0\n";
close(FA_READ);
close(FA_WRITE);
close(FA_READ_SH);
close(FA_WRITE_SH);

# counts of operations
open(COUNTS, ">$tmp_dir/counts.dat") || die("error opening output file: $!\n");
print COUNTS "# P=POSIX, MI=MPI-IO indep., MC=MPI-IO coll., R=read, W=write\n";
print COUNTS "# PR, MIR, MCR, PW, MIW, MCW, Popen, Pseek, Pstat\n";
my $total_syncs = $summary{CP_POSIX_FSYNCS} + $summary{CP_POSIX_FDSYNCS};
print COUNTS "Read, ", $summary{CP_POSIX_READS}, ", ",
    $summary{CP_INDEP_READS}, ", ", $summary{CP_COLL_READS}, "\n",
    "Write, ", $summary{CP_POSIX_WRITES}, ", ", 
    $summary{CP_INDEP_WRITES}, ", ", $summary{CP_COLL_WRITES}, "\n",
    "Open, ", $summary{CP_POSIX_OPENS}, ", ", $summary{CP_INDEP_OPENS},", ",
    $summary{CP_COLL_OPENS}, "\n",
    "Stat, ", $summary{CP_POSIX_STATS}, ", 0, 0\n",
    "Seek, ", $summary{CP_POSIX_SEEKS}, ", 0, 0\n",
    "Mmap, ", $summary{CP_POSIX_MMAPS}, ", 0, 0\n",
    "Fsync, ", $total_syncs, ", 0, 0\n";
close COUNTS;

# histograms of reads and writes
open (HIST, ">$tmp_dir/hist.dat") || die("error opening output file: $!\n");
print HIST "# size_range read write\n";
print HIST "0-100, ", $summary{CP_SIZE_READ_0_100}, ", ",
                 $summary{CP_SIZE_WRITE_0_100}, "\n";
print HIST "101-1K, ", $summary{CP_SIZE_READ_100_1K}, ", ",
                 $summary{CP_SIZE_WRITE_100_1K}, "\n";
print HIST "1K-10K, ", $summary{CP_SIZE_READ_1K_10K}, ", ",
                 $summary{CP_SIZE_WRITE_1K_10K}, "\n";
print HIST "10K-100K, ", $summary{CP_SIZE_READ_10K_100K}, ", ",
                 $summary{CP_SIZE_WRITE_10K_100K}, "\n";
print HIST "100K-1M, ", $summary{CP_SIZE_READ_100K_1M}, ", ",
                 $summary{CP_SIZE_WRITE_100K_1M}, "\n";
print HIST "1M-4M, ", $summary{CP_SIZE_READ_1M_4M}, ", ",
                 $summary{CP_SIZE_WRITE_1M_4M}, "\n";
print HIST "4M-10M, ", $summary{CP_SIZE_READ_4M_10M}, ", ",
                 $summary{CP_SIZE_WRITE_4M_10M}, "\n";
print HIST "10M-100M, ", $summary{CP_SIZE_READ_10M_100M}, ", ",
                 $summary{CP_SIZE_WRITE_10M_100M}, "\n";
print HIST "100M-1G, ", $summary{CP_SIZE_READ_100M_1G}, ", ",
                 $summary{CP_SIZE_WRITE_100M_1G}, "\n";
print HIST "1G+, ", $summary{CP_SIZE_READ_1G_PLUS}, ", ",
                 $summary{CP_SIZE_WRITE_1G_PLUS}, "\n";
close HIST;

# sequential and consecutive accesses
open (PATTERN, ">$tmp_dir/pattern.dat") || die("error opening output file: $!\n");
print PATTERN "# op total sequential consecutive\n";
print PATTERN "Read, ", $summary{CP_POSIX_READS}, ", ",
    $summary{CP_SEQ_READS}, ", ", $summary{CP_CONSEC_READS}, "\n";
print PATTERN "Write, ", $summary{CP_POSIX_WRITES}, ", ",
    $summary{CP_SEQ_WRITES}, ", ", $summary{CP_CONSEC_WRITES}, "\n";
close PATTERN;

# aligned I/O
open (ALIGN, ">$tmp_dir/align.dat") || die("error opening output file: $!\n");
print ALIGN "# total unaligned_mem unaligned_file align_mem align_file\n";
print ALIGN $summary{CP_POSIX_READS} + $summary{CP_POSIX_WRITES}, ", ",
    $summary{CP_MEM_NOT_ALIGNED}, ", ", $summary{CP_FILE_NOT_ALIGNED}, "\n";
close ALIGN;

# MPI types
open (TYPES, ">$tmp_dir/types.dat") || die("error opening output file: $!\n");
print TYPES "# type use_count\n";
print TYPES "Named, ", $summary{CP_COMBINER_NAMED}, "\n";
print TYPES "Dup, ", $summary{CP_COMBINER_DUP}, "\n";
print TYPES "Contig, ", $summary{CP_COMBINER_CONTIGUOUS}, "\n";
print TYPES "Vector, ", $summary{CP_COMBINER_VECTOR}, "\n";
print TYPES "HvecInt, ", $summary{CP_COMBINER_HVECTOR_INTEGER}, "\n";
print TYPES "Hvector, ", $summary{CP_COMBINER_HVECTOR}, "\n";
print TYPES "Indexed, ", $summary{CP_COMBINER_INDEXED}, "\n";
print TYPES "HindInt, ", $summary{CP_COMBINER_HINDEXED_INTEGER}, "\n";
print TYPES "Hindexed, ", $summary{CP_COMBINER_HINDEXED}, "\n";
print TYPES "IndBlk, ", $summary{CP_COMBINER_INDEXED_BLOCK}, "\n";
print TYPES "StructInt, ", $summary{CP_COMBINER_STRUCT_INTEGER}, "\n";
print TYPES "Struct, ", $summary{CP_COMBINER_STRUCT}, "\n";
print TYPES "Subarray, ", $summary{CP_COMBINER_SUBARRAY}, "\n";
print TYPES "Darray, ", $summary{CP_COMBINER_DARRAY}, "\n";
print TYPES "F90Real, ", $summary{CP_COMBINER_F90_REAL}, "\n";
print TYPES "F90Complex, ", $summary{CP_COMBINER_F90_COMPLEX}, "\n";
print TYPES "F90Int, ", $summary{CP_COMBINER_F90_INTEGER}, "\n";
print TYPES "Resized, ", $summary{CP_COMBINER_RESIZED}, "\n";
close TYPES;

# generate histogram of process I/O counts
#
# NOTE: NEED TO FILL IN ACTUAL WRITE DATA!!!
#
$minprocread = (defined $procreads[0]) ? $procreads[0] : 0;
$maxprocread = (defined $procreads[0]) ? $procreads[0] : 0;
for ($i=1; $i < $nprocs; $i++) {
    $rdi = (defined $procreads[$i]) ? $procreads[$i] : 0;
    $minprocread = ($rdi > $minprocread) ? $minprocread : $rdi;
    $maxprocread = ($rdi < $maxprocread) ? $maxprocread : $rdi;
}
$minprocread += $procreads[$nprocs];
$maxprocread += $procreads[$nprocs];
# print "$minprocread $maxprocread\n";

@bucket = ( 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 );

for ($i=0; $i < $nprocs; $i++) {
    $mysize = ((defined $procreads[$i]) ? $procreads[$i] : 0) +
	$procreads[$nprocs];
    $mysize -= $minprocread;
    $mybucket = ($mysize > 0) ?
	(($mysize * 10) / ($maxprocread - $minprocread)) : 0;
    $bucket[$mybucket]++;
}

open(IODIST, ">$tmp_dir/iodist.dat") || die("error opening output file: $!\n");
print IODIST "# bucket n_procs_rd n_procs_wr\n";
print IODIST "# NOTE: WRITES ARE A COPY OF READS FOR NOW!!!\n";

$bucketsize = $maxprocread - $minprocread / 10;
# TODO: do writes also, is dropping a 0 in for now
for ($i=0; $i < 10; $i++) {
    print IODIST $bucketsize * $i + $minprocread, "-",
    $bucketsize * ($i+1) + $minprocread, ", ", $bucket[$i], ", 0\n";
}
close IODIST;

# generate title for summary
($executable, $junk) = split(' ', $cmdline, 2);
@parts = split('/', $executable);
$cmd = $parts[$#parts];

@timearray = localtime($starttime);
$year = $timearray[5] + 1900;
$mon = $timearray[4] + 1;
$mday = $timearray[3];

open(TITLE, ">$tmp_dir/title.tex") || die("error opening output file:$!\n");
print TITLE "
\\rhead{\\thepage\\ of \\pageref{LastPage}}
\\chead[
\\large $cmd ($mon/$mday/$year)
]
{
\\large $cmd ($mon/$mday/$year)
}
\\cfoot[
\\scriptsize{$cmdline}
]
{
\\scriptsize{$cmdline}
}
";
close TITLE;

open(TABLES, ">$tmp_dir/job-table.tex") || die("error opening output file:$!\n");
print TABLES "
\\begin{tabular}{|p{.47\\columnwidth}|p{.35\\columnwidth}|p{.47\\columnwidth}|p{.6\\columnwidth}|}
\\hline
jobid: $jobid \& uid: $uid \& nprocs: $nprocs \& runtime: $runtime seconds\\\\
\\hline
\\end{tabular}
";
close TABLES;

open(TABLES, ">$tmp_dir/access-table.tex") || die("error opening output file:$!\n");
print TABLES "
\\begin{tabular}{r|r}
\\multicolumn{2}{c}{ } \\\\
\\multicolumn{2}{c}{Most Common Access Sizes} \\\\
\\hline
access size \& count \\\\
\\hline
\\hline
";

# sort access sizes (descending)
my $i = 0;
foreach $value (sort {$access_hash{$b} <=> $access_hash{$a} } keys %access_hash) {
    if($i == 4) {
        last;
    }
    if($access_hash{$value} == 0) {
        last;
    }
    print TABLES "$value \& $access_hash{$value} \\\\\n";
    $i++;
}

print TABLES "
\\hline
\\end{tabular}
";
close TABLES;

open(TABLES, ">$tmp_dir/file-count-table.tex") || die("error opening output file:$!\n");
print TABLES "
\\begin{tabular}{r|r|r|r}
\\multicolumn{4}{c}{ } \\\\
\\multicolumn{4}{c}{File Count Summary} \\\\
\\hline
type \& number of files \& avg. size \& max size \\\\
\\hline
\\hline
";
my $counter;
my $sum;
my $max;
my $key;
my $avg;

$counter = 0;
$sum = 0;
$max = 0;
foreach $key (keys %hash_files) {
    $counter++;
    if($hash_files{$key}{'min_open_size'} >
        $hash_files{$key}{'max_size'})
    {
        $sum += $hash_files{$key}{'min_open_size'};
        if($hash_files{$key}{'min_open_size'} > $max)
        {
            $max = $hash_files{$key}{'min_open_size'};
        }
    }
    else
    {
        $sum += $hash_files{$key}{'max_size'};
        if($hash_files{$key}{'max_size'} > $max)
        {
            $max = $hash_files{$key}{'max_size'};
        }
    }
}
if($counter > 0) { $avg = $sum / $counter; }
else { $avg = 0; }
$avg = format_bytes($avg);
$max = format_bytes($max);
print TABLES "total opened \& $counter \& $avg \& $max \\\\\n";

$counter = 0;
$sum = 0;
$max = 0;
foreach $key (keys %hash_files) {
    if($hash_files{$key}{'was_read'} && !($hash_files{$key}{'was_written'}))
    {
        $counter++;
        if($hash_files{$key}{'min_open_size'} >
            $hash_files{$key}{'max_size'})
        {
            $sum += $hash_files{$key}{'min_open_size'};
            if($hash_files{$key}{'min_open_size'} > $max)
            {
                $max = $hash_files{$key}{'min_open_size'};
            }
        }
        else
        {
            $sum += $hash_files{$key}{'max_size'};
            if($hash_files{$key}{'max_size'} > $max)
            {
                $max = $hash_files{$key}{'max_size'};
            }
        }
    }
}
if($counter > 0) { $avg = $sum / $counter; }
else { $avg = 0; }
$avg = format_bytes($avg);
$max = format_bytes($max);
print TABLES "read-only files \& $counter \& $avg \& $max \\\\\n";

$counter = 0;
$sum = 0;
$max = 0;
foreach $key (keys %hash_files) {
    if(!($hash_files{$key}{'was_read'}) && $hash_files{$key}{'was_written'})
    {
        $counter++;
        if($hash_files{$key}{'min_open_size'} >
            $hash_files{$key}{'max_size'})
        {
            $sum += $hash_files{$key}{'min_open_size'};
            if($hash_files{$key}{'min_open_size'} > $max)
            {
                $max = $hash_files{$key}{'min_open_size'};
            }
        }
        else
        {
            $sum += $hash_files{$key}{'max_size'};
            if($hash_files{$key}{'max_size'} > $max)
            {
                $max = $hash_files{$key}{'max_size'};
            }
        }
    }
}
if($counter > 0) { $avg = $sum / $counter; }
else { $avg = 0; }
$avg = format_bytes($avg);
$max = format_bytes($max);
print TABLES "write-only files \& $counter \& $avg \& $max \\\\\n";

$counter = 0;
$sum = 0;
$max = 0;
foreach $key (keys %hash_files) {
    if($hash_files{$key}{'was_read'} && $hash_files{$key}{'was_written'})
    {
        $counter++;
        if($hash_files{$key}{'min_open_size'} >
            $hash_files{$key}{'max_size'})
        {
            $sum += $hash_files{$key}{'min_open_size'};
            if($hash_files{$key}{'min_open_size'} > $max)
            {
                $max = $hash_files{$key}{'min_open_size'};
            }
        }
        else
        {
            $sum += $hash_files{$key}{'max_size'};
            if($hash_files{$key}{'max_size'} > $max)
            {
                $max = $hash_files{$key}{'max_size'};
            }
        }
    }
}
if($counter > 0) { $avg = $sum / $counter; }
else { $avg = 0; }
$avg = format_bytes($avg);
$max = format_bytes($max);
print TABLES "read/write files \& $counter \& $avg \& $max \\\\\n";

$counter = 0;
$sum = 0;
$max = 0;
foreach $key (keys %hash_files) {
    if($hash_files{$key}{'was_written'} &&
        $hash_files{$key}{'min_open_size'} == 0 &&
        $hash_files{$key}{'max_size'} > 0)
    {
        $counter++;
        if($hash_files{$key}{'min_open_size'} >
            $hash_files{$key}{'max_size'})
        {
            $sum += $hash_files{$key}{'min_open_size'};
            if($hash_files{$key}{'min_open_size'} > $max)
            {
                $max = $hash_files{$key}{'min_open_size'};
            }
        }
        else
        {
            $sum += $hash_files{$key}{'max_size'};
            if($hash_files{$key}{'max_size'} > $max)
            {
                $max = $hash_files{$key}{'max_size'};
            }
        }
    }
}
if($counter > 0) { $avg = $sum / $counter; }
else { $avg = 0; }
$avg = format_bytes($avg);
$max = format_bytes($max);
print TABLES "created files \& $counter \& $avg \& $max \\\\\n";

print TABLES "
\\hline
\\end{tabular}
";
close(TABLES);


#
# Generate Per Filesystem Data
#
open(TABLES, ">$tmp_dir/fs-data-table.tex") || die("error opening output files:$!\n");
print TABLES "
\\begin{tabular}{c|r|r|r|r}
\\multicolumn{5}{c}{ } \\\\
\\multicolumn{5}{c}{Data Transfer Per Filesystem} \\\\
\\hline
\\multirow{2}{*}{File System} \& \\multicolumn{2}{c}{Write} \\vline \& \\multicolumn{2}{c}{Read} \\\\
\\cline{2-5}
\& MiB \& Ratio \& MiB \& Ratio \\\\\
\\hline
\\hline
";
foreach $key (keys %fs_data)
{
    my $wr_total_mb = ($fs_data{$key}->[1] / (1024*1024));
    my $rd_total_mb = ($fs_data{$key}->[0] / (1024*1024));
    my $wr_total_rt;

    if ($cumul_write_bytes_shared+$cumul_write_bytes_shared)
    {
        $wr_total_rt = ($fs_data{$key}->[1] / ($cumul_write_bytes_shared + $cumul_write_bytes_indep));
    }
    else
    {
        $wr_total_rt = 0;
    }

    my $rd_total_rt;
    if ($cumul_write_bytes_shared+$cumul_read_bytes_indep)
    {
        $rd_total_rt = ($fs_data{$key}->[0] / ($cumul_read_bytes_shared + $cumul_read_bytes_indep));
    }
    else
    {
        $rd_total_rt = 0;
    }

    printf TABLES "%s \& %.5f \& %.5f \& %.5f \& %.5f \\\\\n",
        $key, $wr_total_mb, $wr_total_rt, $rd_total_mb, $rd_total_rt;
}
print TABLES "
\\hline
\\end{tabular}
";
close(TABLES);


open(TIME, ">$tmp_dir/time-summary.dat") || die("error opening output file:$!\n");
print TIME "# <type>, <app time>, <read>, <write>, <meta>\n";
print TIME "POSIX, ", ((($runtime * $nprocs - $summary{CP_F_POSIX_READ_TIME} -
    $summary{CP_F_POSIX_WRITE_TIME} -
    $summary{CP_F_POSIX_META_TIME})/($runtime * $nprocs)) * 100);
print TIME ", ", (($summary{CP_F_POSIX_READ_TIME}/($runtime * $nprocs))*100);
print TIME ", ", (($summary{CP_F_POSIX_WRITE_TIME}/($runtime * $nprocs))*100);
print TIME ", ", (($summary{CP_F_POSIX_META_TIME}/($runtime * $nprocs))*100), "\n";
print TIME "MPI-IO, ", ((($runtime * $nprocs - $summary{CP_F_MPI_READ_TIME} -
    $summary{CP_F_MPI_WRITE_TIME} -
    $summary{CP_F_MPI_META_TIME})/($runtime * $nprocs)) * 100);
print TIME ", ", (($summary{CP_F_MPI_READ_TIME}/($runtime * $nprocs))*100);
print TIME ", ", (($summary{CP_F_MPI_WRITE_TIME}/($runtime * $nprocs))*100);
print TIME ", ", (($summary{CP_F_MPI_META_TIME}/($runtime * $nprocs))*100), "\n";
close TIME;

# copy template files to tmp tmp_dir
system "cp $FindBin::Bin/../share/*.gplt $tmp_dir/";
system "cp $FindBin::Bin/../share/*.tex $tmp_dir/";

# generate template for file access plot (we have to set range)
my $ymax = $nprocs + 1;
open(FILEACC, ">$tmp_dir/file-access-read-eps.gplt") || die("error opening output file:$!\n");
print FILEACC "#!/usr/bin/gnuplot -persist

set terminal postscript eps color solid font \"Helvetica\" 18 size 10in,2.5in
set output \"file-access-read.eps\"
set ylabel \"MPI rank\"
set xlabel \"hours:minutes:seconds\"
set xdata time
set timefmt \"%s\"
set format x \"%H:%M:%S\"
set yrange [-1:$ymax]
set title \"Timespan from first to last read access on independent files\"
# the xrange doesn't work for some reason
#set xrange [0:$runtime]
#set ytics -1,1
set lmargin 4

# color blindness work around
set style line 2 lc 3
set style line 3 lc 4
set style line 4 lc 5
set style line 5 lc 2
set style increment user

# lw 3 to make lines thicker...
# note that writes are slightly offset for better visibility
plot \"file-access-read.dat\" using 1:2:3:4 with vectors nohead filled notitle
";
close FILEACC;

open(FILEACC, ">$tmp_dir/file-access-write-eps.gplt") || die("error opening output file:$!\n");
print FILEACC "#!/usr/bin/gnuplot -persist

set terminal postscript eps color solid font \"Helvetica\" 18 size 10in,2.5in
set output \"file-access-write.eps\"
set ylabel \"MPI rank\"
set xlabel \"hours:minutes:seconds\"
set xdata time
set timefmt \"%s\"
set format x \"%H:%M:%S\"
set title \"Timespan from first to last write access on independent files\"
set yrange [-1:$ymax]
# the xrange doesn't work for some reason
# set xrange [0:$runtime]
#set ytics -1,1
set lmargin 4

# color blindness work around
set style line 2 lc 3
set style line 3 lc 4
set style line 4 lc 5
set style line 5 lc 2
set style increment user

# lw 3 to make lines thicker...
plot \"file-access-write.dat\" using 1:2:3:4 with vectors nohead filled lt 2 notitle
";
close FILEACC;

open(FILEACC, ">$tmp_dir/file-access-shared-eps.gplt") || die("error opening output file:$!\n");
print FILEACC "#!/usr/bin/gnuplot -persist

set terminal postscript eps color solid font \"Helvetica\" 18 size 10in,2.5in
set output \"file-access-shared.eps\"
set xlabel \"hours:minutes:seconds\"
set xdata time
set timefmt \"%s\"
set format x \"%H:%M:%S\"
unset ytics
set ylabel \"All processes\"
# the xrange doesn't work for some reason
# set xrange [0:$runtime]
set yrange [-1:1]
set title \"Timespan from first to last access on files shared by all processes\"
set lmargin 4

# color blindness work around
set style line 2 lc 3
set style line 3 lc 4
set style line 4 lc 5
set style line 5 lc 2
set style increment user

plot \"file-access-read-sh.dat\" using 1:2:3:4 with vectors nohead filled lw 10 title \"read\", \\
\"file-access-write-sh.dat\" using 1:((\$2)-.2):3:4 with vectors nohead filled lw 10 title \"write\"
";
close FILEACC;

$cumul_read_indep /= $nprocs;
$cumul_read_bytes_indep /= $nprocs;
$cumul_read_bytes_indep /= 1048576.0;

$cumul_write_indep /= $nprocs;
$cumul_write_bytes_indep /= $nprocs;
$cumul_write_bytes_indep /= 1048576.0;

$cumul_read_shared /= $nprocs;
$cumul_read_bytes_shared /= $nprocs;
$cumul_read_bytes_shared /= 1048576.0;

$cumul_write_shared /= $nprocs;
$cumul_write_bytes_shared /= $nprocs;
$cumul_write_bytes_shared /= 1048576.0;

$cumul_meta_shared /= $nprocs;
$cumul_meta_indep /= $nprocs;

open(FILEACC, ">$tmp_dir/file-access-table.tex") || die("error opening output file:$!\n");
print FILEACC "
\\begin{tabular}{l|p{1.7in}r}
\\multicolumn{3}{c}{Average I/O per process} \\\\
\\hline
 \& Cumulative time spent in I/O functions (seconds) \& Amount of I/O (MB) \\\\
\\hline
\\hline
";

# printf to get consistent precision in output
printf(FILEACC "Independent reads \& \\multicolumn{1}{r}{%f} \& \\multicolumn{1}{r}{%f} \\\\", 
    $cumul_read_indep, $cumul_read_bytes_indep);
printf(FILEACC "Independent writes \& \\multicolumn{1}{r}{%f} \& \\multicolumn{1}{r}{%f} \\\\", 
    $cumul_write_indep, $cumul_write_bytes_indep);
printf(FILEACC "Independent metadata \& \\multicolumn{1}{r}{%f} \& \\multicolumn{1}{r}{N/A} \\\\", 
    $cumul_meta_indep);
printf(FILEACC "Shared reads \& \\multicolumn{1}{r}{%f} \& \\multicolumn{1}{r}{%f} \\\\", 
    $cumul_read_shared, $cumul_read_bytes_shared);
printf(FILEACC "Shared writes \& \\multicolumn{1}{r}{%f} \& \\multicolumn{1}{r}{%f} \\\\", 
    $cumul_write_shared, $cumul_write_bytes_shared);
printf(FILEACC "Shared metadata \& \\multicolumn{1}{r}{%f} \& \\multicolumn{1}{r}{N/A} \\\\", 
    $cumul_meta_shared);

print FILEACC "
\\hline
\\end{tabular}
";
close(FILEACC);

#
# Variance Data
#
open(VARP, ">$tmp_dir/variance-table.tex") || die("error opening output file:$!\n");
print VARP "
\\begin{tabular}{c|r|r|r|r|r|r|r|r|r}
\\multicolumn{10}{c}{} \\\\
\\multicolumn{10}{c}{Variance in Shared Files} \\\\
\\hline
File \& Processes \& \\multicolumn{3}{c}{Fastest} \\vline \&
\\multicolumn{3}{c}{Slowest} \\vline \& \\multicolumn{2}{c}{\$\\sigma\$} \\\\
\\cline{3-10}
Suffix \&  \& Rank \& Time \& Bytes \& Rank \& Time \& Bytes \& Time \& Bytes \\\\
\\hline
\\hline
";

foreach $key (keys %hash_files) {
    if ($hash_files{$key}{'procs'} > 1)
    {
        my $vt = sprintf("%.3g", sqrt($hash_files{$key}{'variance_time'}));
        my $vb = sprintf("%.3g", sqrt($hash_files{$key}{'variance_bytes'}));
        my $fast_bytes = format_bytes($hash_files{$key}{'fastest_bytes'});
        my $slow_bytes = format_bytes($hash_files{$key}{'slowest_bytes'});

        print VARP "
               $hash_files{$key}{'name'} \&
               $hash_files{$key}{'procs'} \&
               $hash_files{$key}{'fastest_rank'} \&
               $hash_files{$key}{'fastest_time'} \&
               $fast_bytes \&
               $hash_files{$key}{'slowest_rank'} \&
               $hash_files{$key}{'slowest_time'} \& 
               $slow_bytes \&
               $vt \&
               $vb \\\\
         ";
    }
}

print VARP "
\\hline
\\end{tabular}
";
close(VARP);

if(-x "$FindBin::Bin/gnuplot")
{
    $gnuplot = "$FindBin::Bin/gnuplot";
}
else
{
    $gnuplot = "gnuplot";
}

# move to tmp_dir
chdir $tmp_dir;

# execute gnuplot scripts
system "$gnuplot counts-eps.gplt";
system "epstopdf counts.eps";
system "$gnuplot hist-eps.gplt";
system "epstopdf hist.eps";
system "$gnuplot pattern-eps.gplt";
system "epstopdf pattern.eps";
system "$gnuplot time-summary-eps.gplt";
system "epstopdf time-summary.eps";
system "$gnuplot file-access-read-eps.gplt";
system "epstopdf file-access-read.eps";
system "$gnuplot file-access-write-eps.gplt";
system "epstopdf file-access-write.eps";
system "$gnuplot file-access-shared-eps.gplt";
system "epstopdf file-access-shared.eps";

#system "gnuplot align-pdf.gplt";
#system "gnuplot iodist-pdf.gplt";
#system "gnuplot types-pdf.gplt";

# generate summary PDF
system "pdflatex -halt-on-error summary.tex > latex.output";
system "pdflatex -halt-on-error summary.tex > latex.output2";

# get back out of tmp dir and grab results
chdir $orig_dir;
system "mv $tmp_dir/summary.pdf $output_file";

sub process_file_record
{
    my $rank = $_[0];
    my $hash = $_[1];
    my(%file_record) = %{$_[2]};

    if($file_record{'CP_INDEP_OPENS'} == 0 &&
        $file_record{'CP_COLL_OPENS'} == 0 &&
        $file_record{'CP_POSIX_OPENS'} == 0)
    {
        # file wasn't really opened, just stat probably
        return;
    }

    # record smallest open time size reported by any rank
    if(!defined($hash_files{$hash}{'min_open_size'}) ||
        $hash_files{$hash}{'min_open_size'} > 
        $file_record{'CP_SIZE_AT_OPEN'})
    {
        $hash_files{$hash}{'min_open_size'} = 
            $file_record{'CP_SIZE_AT_OPEN'};
    }

    # record largest size that the file reached at any rank
    if(!defined($hash_files{$hash}{'max_size'}) ||
        $hash_files{$hash}{'max_size'} <  
        ($file_record{'CP_MAX_BYTE_READ'} + 1))
    {
        $hash_files{$hash}{'max_size'} = 
            $file_record{'CP_MAX_BYTE_READ'} + 1;
    }
    if(!defined($hash_files{$hash}{'max_size'}) ||
        $hash_files{$hash}{'max_size'} <  
        ($file_record{'CP_MAX_BYTE_WRITTEN'} + 1))
    {
        $hash_files{$hash}{'max_size'} = 
            $file_record{'CP_MAX_BYTE_WRITTEN'} + 1;
    }

    # make sure there is an initial value for read and write flags
    if(!defined($hash_files{$hash}{'was_read'}))
    {
        $hash_files{$hash}{'was_read'} = 0;
    }
    if(!defined($hash_files{$hash}{'was_written'}))
    {
        $hash_files{$hash}{'was_written'} = 0;
    }

    if($file_record{'CP_INDEP_OPENS'} > 0 ||
        $file_record{'CP_COLL_OPENS'} > 0)
    {
        # mpi file
        if($file_record{'CP_INDEP_READS'} > 0 ||
            $file_record{'CP_COLL_READS'} > 0 ||
            $file_record{'CP_SPLIT_READS'} > 0 ||
            $file_record{'CP_NB_READS'} > 0)
        {
            # data was read from the file
            $hash_files{$hash}{'was_read'} = 1;
        }
        if($file_record{'CP_INDEP_WRITES'} > 0 ||
            $file_record{'CP_COLL_WRITES'} > 0 ||
            $file_record{'CP_SPLIT_WRITES'} > 0 ||
            $file_record{'CP_NB_WRITES'} > 0)
        {
            # data was written to the file
            $hash_files{$hash}{'was_written'} = 1;
        }
    }
    else
    {
        # posix file
        if($file_record{'CP_POSIX_READS'} > 0 ||
            $file_record{'CP_POSIX_FREADS'} > 0)
        {
            # data was read from the file
            $hash_files{$hash}{'was_read'} = 1;
        }
        if($file_record{'CP_POSIX_WRITES'} > 0 ||
            $file_record{'CP_POSIX_FWRITES'} > 0)
        {
            # data was written to the file 
            $hash_files{$hash}{'was_written'} = 1;
        }
    }

    $hash_files{$hash}{'name'} = $file_record{CP_NAME_SUFFIX};

    if ($rank == -1)
    {
        $hash_files{$hash}{'procs'}          = $nprocs;
        $hash_files{$hash}{'slowest_rank'}   = $file_record{'CP_SLOWEST_RANK'};
        $hash_files{$hash}{'slowest_time'}   = $file_record{'CP_F_SLOWEST_RANK_TIME'};
        $hash_files{$hash}{'slowest_bytes'}  = $file_record{'CP_SLOWEST_RANK_BYTES'};
        $hash_files{$hash}{'fastest_rank'}   = $file_record{'CP_FASTEST_RANK'};
        $hash_files{$hash}{'fastest_time'}   = $file_record{'CP_F_FASTEST_RANK_TIME'};
        $hash_files{$hash}{'fastest_bytes'}  = $file_record{'CP_FASTEST_RANK_BYTES'};
        $hash_files{$hash}{'variance_time'}  = $file_record{'CP_F_VARIANCE_RANK_TIME'};
        $hash_files{$hash}{'variance_bytes'} = $file_record{'CP_F_VARIANCE_RANK_BYTES'};
    }
    else
    {
        my $total_time = $file_record{'CP_F_POSIX_META_TIME'} +
                         $file_record{'CP_F_POSIX_READ_TIME'} +
                         $file_record{'CP_F_POSIX_WRITE_TIME'};

        my $total_bytes = $file_record{'CP_BYTES_READ'} +
                          $file_record{'CP_BYTES_WRITTEN'};

        if(!defined($hash_files{$hash}{'slowest_time'}) ||
           $hash_files{$hash}{'slowest_time'} < $total_time)
        {
            $hash_files{$hash}{'slowest_time'}  = $total_time;
            $hash_files{$hash}{'slowest_rank'}  = $rank;
            $hash_files{$hash}{'slowest_bytes'} = $total_bytes;
        }

        if(!defined($hash_files{$hash}{'fastest_time'}) ||
           $hash_files{$hash}{'fastest_time'} > $total_time)
        {
            $hash_files{$hash}{'fastest_time'}  = $total_time;
            $hash_files{$hash}{'fastest_rank'}  = $rank;
            $hash_files{$hash}{'fastest_bytes'} = $total_bytes;
        }

        if(!defined($hash_files{$hash}{'variance_time_S'}))
        {
            $hash_files{$hash}{'variance_time_S'} = 0;
            $hash_files{$hash}{'variance_time_T'} = $total_time;
            $hash_files{$hash}{'variance_time_n'} = 1;
            $hash_files{$hash}{'variance_bytes_S'} = 0;
            $hash_files{$hash}{'variance_bytes_T'} = $total_bytes;
            $hash_files{$hash}{'variance_bytes_n'} = 1;
            $hash_files{$hash}{'procs'} = 1;
            $hash_files{$hash}{'variance_time'} = 0;
            $hash_files{$hash}{'variance_bytes'} = 0;
        }
        else
        {
            my $n = $hash_files{$hash}{'variance_time_n'};
            my $m = 1;
            my $T = $hash_files{$hash}{'variance_time_T'};
            $hash_files{$hash}{'variance_time_S'} += ($m/($n*($n+$m)))*(($n/$m)*$total_time - $T)*(($n/$m)*$total_time - $T);
            $hash_files{$hash}{'variance_time_T'} += $total_time;
            $hash_files{$hash}{'variance_time_n'} += 1;

            $hash_files{$hash}{'variance_time'}    = $hash_files{$hash}{'variance_time_S'} / $hash_files{$hash}{'variance_time_n'};

            $n = $hash_files{$hash}{'variance_bytes_n'};
            $m = 1;
            $T = $hash_files{$hash}{'variance_bytes_T'};
            $hash_files{$hash}{'variance_bytes_S'} += ($m/($n*($n+$m)))*(($n/$m)*$total_bytes - $T)*(($n/$m)*$total_bytes - $T);
            $hash_files{$hash}{'variance_bytes_T'} += $total_bytes;
            $hash_files{$hash}{'variance_bytes_n'} += 1;

            $hash_files{$hash}{'variance_bytes'}    = $hash_files{$hash}{'variance_bytes_S'} / $hash_files{$hash}{'variance_bytes_n'};

            $hash_files{$hash}{'procs'} = $n;
        }
    }

    # TODO 
    # (detect mpi or posix and):
    # - sum meta time per rank for uniq files
    # - sum io time per rank for uniq files
    # - sum time from first open to last io for shared files
    # - sum meta time/nprocs for shared files
    # - sum io time/nprocs for shared files
    
    # TODO: ideas
    # graph time spent performing I/O per rank
    # for rank that spent the most time performing I/O:
    # - meta on ro files, meta on wo files, read time, write time
    # table with nfiles accessed, ro, wo, rw, created
}

sub process_args
{
    use vars qw( $opt_help $opt_output );

    Getopt::Long::Configure("no_ignore_case", "bundling");
    GetOptions( "help",
        "output=s");

    if($opt_help)
    {
        print_help();
        exit(0);
    }

    if($opt_output)
    {
        $output_file = $opt_output;
    }

    # there should only be one remaining argument: the input file 
    if($#ARGV != 0)
    {
        print "Error: invalid arguments.\n";
        print_help();
        exit(1);
    }
    $input_file = $ARGV[0];

    return;
}

sub print_help
{
    print <<EOF;

Usage: $PROGRAM_NAME <options> input_file

    --help          Prints this help message
    --output        Specifies a file to write pdf output to
                    (defaults to ./summary.pdf)
Purpose:

    This script reads a Darshan output file generated by a job and
    generates a pdf file summarizing job behavior.

EOF
    return;
}
