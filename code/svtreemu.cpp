// run maximum likelihood inference


#include <fstream>
#include <string>
#include <cstring>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <algorithm>
#include <iostream>
#include <iomanip>
#include <string.h>
#include <vector>
#include <random>
#include <sstream>
#include <ctime>
#include <map>
#include <unistd.h>

#include <gsl/gsl_rng.h>
#include <gsl/gsl_randist.h>
#include <gsl/gsl_statistics.h>
#include <gsl/gsl_multimin.h>

#include <boost/program_options.hpp>

#include "gzstream.h"

#include "evo_tree.hpp"
#include "genome.hpp"
#include "stats.hpp"
#include "utilities.hpp"

using namespace std;

int debug = 0;
const int CN_MAX = 4;

// global values for gsl function minimization
//vector<vector<int> > vobs;
//vector<double> tobs;
//int Ns;
//int Nchar;

int main (int argc, char ** const argv) {
  int miter, nmax;
  double tolerance, ssize, mu_0, vlnorm;
  string datafile, timefile, treefile, ofile;

  namespace po = boost::program_options;
  po::options_description generic("Generic options");
  generic.add_options()
    ("version,v", "print version string")
    ("help,h", "produce help message")
    ;
  po::options_description required("Required parameters");
  required.add_options()
     ("cfile,c", po::value<string>(&datafile)->required(), "input copy number profile file")
     ("tfile,t", po::value<string>(&timefile)->required(), "input time information file")
     ("pfile,p", po::value<string>(&timefile)->required(), "input tree information file")
     ;
  po::options_description optional("Optional parameters");
  optional.add_options()
    ("nsample,s", po::value<int>(&Ns)->default_value(5), "number of samples or regions")
    ("tolerance,r", po::value<double>(&tolerance)->default_value(1e-2), "tolerance value")
    ("miter,m", po::value<int>(&miter)->default_value(2000), "maximum number of iterations in maximization")
    ("nmax,n", po::value<int>(&nmax)->default_value(100), "number of maximizations to attempt")
    ("ssize,z", po::value<double>(&ssize)->default_value(0.01), "initial step size")
    ("ofile,o", po::value<string>(&ofile)->default_value("results-maxL-mu-tree.txt"), "output tree file")
    ("mu,x", po::value<double>(&mu_0)->default_value(1.0), "initial mutation rate estimate (SCA/locus/time)")
    ("vlnorm,l", po::value<double>(&vlnorm)->default_value(1.0), "scale of lognorm for initial value sampling")
    ;

  po::options_description cmdline_options;
  cmdline_options.add(generic).add(required).add(optional);
  po::variables_map vm;

  try {
      po::store(po::command_line_parser(argc, argv).options(cmdline_options).run(), vm);

      if(vm.count("help")){
          cout << cmdline_options << endl;
          return 1;
      }

      if(vm.count("version")){
          cout << "svtreeml [version 0.1], a program to build a phylogenetic tree from copy number profile" << endl;
          return 1;
      }

      po::notify(vm);

      datafile = vm["cfile"].as<string>();
      timefile = vm["tfile"].as<string>();
      treefile = vm["pfile"].as<string>();
      Ns = vm["nsample"].as<int>();
      tolerance = vm["tolerance"].as<double>();
      miter = vm["miter"].as<int>();
      ssize = vm["ssize"].as<double>();
      // cout << "Input: " << endl;
      // cout << " Data file: " << datafile << endl;
      // cout << " Time file: " << timefile << endl;
      // cout << " Number of samples: " << Ns << endl;
      // cout << " Number of population: " << Npop << endl;
      // cout << " Number of generation: " << Ngen << endl;
      // cout << " Tolerance value: " << tolerance << endl;
  } catch (const std::exception& e) {
        std::cerr << e.what() << std::endl;
        return 1;
  }

  setup_rng(0);


  vector<vector<int> > data = read_data_var_regions(datafile, Ns, CN_MAX);
  Nchar = data.size();

  // tobs already defined globally
  tobs = read_time_info(timefile,Ns);
  
  // read in mle tree
  evo_tree test_tree = read_tree_info(treefile,Ns);
  
  //vector<vector<int> > vobs; // already defined globally
  for(int nc=0; nc<Nchar; ++nc){
    vector<int> obs;
    for(int i=0; i<Ns; ++i){
      obs.push_back( data[nc][i+3] );
    }
    vobs.push_back( obs );
  }
  
  // estimate mutation rate
  //test_tree.print();
  test_tree.tobs = tobs;
  test_tree.mu = mu_0;
  
  //double Ls = get_likelihood(Ns, Nchar, vobs, test_tree);
  //cout << "\nOriginal tree -ve likelihood: " << -Ls << endl;

  cout << "\n\n### Running optimisation: branches constrained, mu free" << endl;
  cout << "vlnorm: " << vlnorm << endl;
  
  double minL = -1*LARGE_LNL;
  evo_tree min_tree_mu;
  
  for(int i=0; i<nmax; ++i){
    double Lf = 0;
    double mu_g;
    if(i==0){
      mu_g = mu_0;
    }else{
      mu_g = gsl_ran_lognormal(r, log(mu_0), vlnorm);
    }
    test_tree.mu = mu_g;
    
    //evo_tree min_tree = max_likelihood(test_tree, Lf, 1, 1);
    evo_tree min_tree = max_likelihood(test_tree, Lf, ssize, tolerance, miter, 1, 1);
    cout << "Testing mu_g / -lnL / mu: " << mu_g << " / " << Lf << " / " << min_tree.mu << endl;
    
    if(Lf < minL){
      minL = Lf;
      min_tree_mu = min_tree;
      //cout << "\nMinimised tree likelihood / mu : " << Lf << "\t" << min_tree_mu.mu*Nchar <<endl;      
      //min_tree_mu.print();
    }
    
  }

  cout << "\nMinimised tree likelihood / Nchar / mu (SCA/locus/time): " << minL << " / " << Nchar << " / " <<  min_tree_mu.mu << endl;
  min_tree_mu.print();
  
  stringstream sstm;
  ofstream out_tree;
  out_tree.open(ofile);
  min_tree_mu.write(out_tree);
  out_tree.close();
  sstm.str("");  
}
