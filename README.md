Structural Variation Evolutionary Tree Analysis
=============

# Introduction
This is a set of programs to simulate and build phylogenetic trees from copy number profiles caused by chromosomal alteration events and structural variations (SVs).
Currently, five types of events are considered, including segment duplication, segment deletion, chromosomal gain, chromosomal loss, and whole genome doubling.

The tree building programs take as input the allele-specific or total copy numbers called from mulitiple samples of a patient.

There are mainly 3 programs:
* sveta: simulating SVs along a phylogenetic (coalescence) tree
* svtreeml: building phylogenetic trees from copy numbers with maximum likelihood approach
* svtreemcmc: building phylogenetic trees from copy numbers with Bayesian MCMC approach


# Installation
This package is mostly written in C++. There are a few scripts written in R and Python, for plotting and text processing.

## Dependencies

* Required C/C++ libraries
  * CMake is required for BFGS optimization
  * C libaries: gsl, boost (version >= 1.42)

* Required R libraries
  * plot-cns.R: `copynumber`, `reshape`, `tools`, `tidyr`, `dplyr`, `purrr`
  * plot-trees-all.R: `ggtree`, `ape`, `tools`, `ggplot2`

* Required Python libraries
  * newick2elist.py: networkx

### How to install CMake

Get the latest “Unix/Linux Source” *.tar.gz file.
```
HOME=~/local
mkdir -p $HOME
tar -xf cmake*.tar.gz
cd cmake*
HOME=~/local
./configure --prefix=$HOME
make
make install
```

### How to required R libraries
```
if (!requireNamespace("BiocManager", quietly = TRUE))
    install.packages("BiocManager")
BiocManager::install()
BiocManager::install("copynumber")
BiocManager::install("ggtree")

install.packages("reshape")
install.packages("ape")

# Check installed packages
installed.packages()[, c("Package", "LibPath")]

```

## Building C++
OpenMP is used to accelerate tree search in svtreeml.
To turned off OpenMP, please set "omp =" in makefile and comment out "#include <omp.h>" in svtreeml.cpp.

To build the C++ code, change into the code directory and type make:
```shell
> cd code
> make
```


## Running
You may use the provided bash scripts to run the programs.

```shell
# Simulating mutations on a coalescence tree
> bash run-sveta.sh
# Build a tree from copy number profile with maximum likelihood method
> bash run-svtreeml.sh
# Build a tree from copy number profile with MCMC method
> bash run-svtreemcmc.sh
```


# Simulation with sveta
There are three Markov models of evolution for the copy number profiles:
* 0: Mk model (extension of JC69 model)
* 1: model of total copy number
* 2: model of allele-specific copy number

There are two ways of simulating mutations along a tree:
1. simulating waiting times along a branch (default)
2. simulating sequences at the end of a branch

Please see run-sveta.sh to learn how to set different parameters

## Input
* --epop Ne: The initial coalescence tree has a expected tree height smaller than 2. Ne can be used to scale the tree height by Ne, by multipling each branch length with Ne.

* --tiff delta_t: On the initial tree, the tip nodes have the same time. This parameter can be used to introduce different times at the tip nodes. The terminal branches are increased by random multiples of delta_t. The maximum multiple is the number of samples.


## Output
* *-cn.txt.gz: The total copy number for each site on each sample
* *-rel-times.txt: The sampling time of tip nodes
* *-allele-cn.txt.gz: The allele-specific copy number for each site on each sample
* *-info.txt: The time of each node and the total number of mutations simulated on each branch of the tree, grouped by lineages of tip nodes.
* *-mut.txt: The list of simulated mutations on each branch of the tree.
* *-tree.txt: The simulated tree in tab-delimited format
* *-tree.nex: The simulated tree in NEWICK format, with branch length reprenting calender time
* *-tree-nmut.nex: The simulated tree in NEWICK format, with branch length reprenting number of mutations

File *-cn.txt.gz can serve as the input to a tree building program that used total copy number.

File *-allele-cn.txt.gz can serve as the input to a tree building program that used allele-specific copy number.

File *-rel-times.txt can provide the timing information of tip nodes to allow etimation of divergence time and mutation rates.

Files *-tree.* provide the real tree, which can be used for measuring the accuracy of tree building programs.

File *-info.txt and *-mut.txt can be used to map mutations onto the tree?



# Tree building with ML
There are 4 running modes in svtreeml.
* mode 0: building maximum likelihood tree from input copy numbers
* mode 1: a simple comprehensive test on a simulated tree
* mode 2: computing likelihood given a tree and its parameters (branch length and mutation rates)
* mode 3: computing maximum likelihood tree given a tree topolgy
The last three modes can be used to validate the computation of likelihood.

There are 3 tree searching method:
* exhaustive search (feasible for trees with fewer than 7 samples)
* hill climbing
* genetic algorithm (may be slow)

<!-- ## How to prepare MP trees -->
The initial trees for tree searching can be obtained by maximum parsimony methods.

Please see run-svtreeml.sh to learn how to set different parameters


# Tree building with MCMC

## Input
* (Required) A file containing copy numbers for all the samples, including the normal sample (*-cn.txt.gz or *-allele-cn.txt.gz)
* (Optional) A file containing the timing information of tip nodes (*-rel-times.txt)
* (Optional) A configuration file which sets most input parameters (mcmc.cfg)

Please see run-svtreemcmc.sh to learn how to set different parameters

There are two runing modes depending on whether a reference tree is provided.
With a reference tree, the tree topolgy is fixed.


## Output
There are two output files:
* trace-mcmc-params_*, which records the traces of parameters
* trace-mcmc-trees_*, which records the sampled trees

<!-- ## How to analyze the results of svtreemcmc -->
trace-mcmc-params_* can be imported into [Tracer](https://beast.community/tracer) to check the convergence of the chains.

trace-mcmc-trees_* can be analyzed by [TreeAnnotator](https://beast.community/treeannotator) to get a summary tree (maximum credibility tree).
