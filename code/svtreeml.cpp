/*
This file builds phylogenetic tree from integer copy numbers of multiple samples with maximum likelihood approach.
*/

#ifdef _OPENMP
#include <omp.h>    // used for accelerating tree search
#endif

#include <boost/program_options.hpp>
#include <boost/filesystem.hpp>

#include "parse_cn.hpp"
#include "nni.hpp"
// #include "optimization.hpp"
#include "state.hpp"


// using namespace std;


// The number of tree for at most 11 samples, become slow with >6 leaves
// int max_tree_num = fact(Ns) * fact(Ns - 1) / exp2(Ns - 1); // For trees
static const int num_trees[] = {1, 1, 3, 15, 105, 945, 10395, 135135, 2027025, 34459425, 654729075};
// The maximum number of samples in the tree which allows exhaustive search
const int LARGE_TREE = 11;
// The number of trees to search before terminating
const int MAX_TREE = 100;
// The maximum number of trees to perturb
const int MAX_TREE1 = 20;
// The maximum number of trees to refine
const int MAX_TREE2 = 5;
// The maximum number of times to refine the final set of trees
const int MAX_PERTURB = 100;
const int MAX_OPT = 10; // max number of optimization for each tree


double loglh_epsilon = 0.001;
double tolerance = 0.01;
int miter = 2000;
int speed_nni = 0;

int debug = 0;

// global value for tree search
map<string, int> searched_trees;
bool exhausted_tree_search = false;

gsl_rng* r;
unsigned seed;


/********* input parameters ***********/
int Ns;   // number of samples
int age;

// Set max_* as global variables to avoid adding more parameters in maximization
int m_max;
int max_wgd;
int max_chr_change;
int max_site_change;

int cn_max;
int is_total; // whether or not the input is total copy number

int model;
int cons;
int maxj;

int use_repeat;   // whether or not to use repeated site patterns, used in get_likelihood_chr*
int correct_bias; // Whether or not to correct acquisition bias, used in get_likelihood_*
int num_invar_bins;   // number of invariant sites

int only_seg; // Whether or not to only consider segment-level mutations, used in get_likelihood_revised

int infer_wgd; // whether or not to infer WGD status of a sample, called in initialize_lnl_table_decomp
int infer_chr; // whether or not to infer chromosome gain/loss status of a sample, called in initialize_lnl_table_decomp

int nstate;

/********* derived from input ***********/
map<int, vector<vector<int>>> vobs;   // CNP for each site, grouped by chr
vector<double> tobs; // input times for each sample, should be the same for all trees, defined when reading input, used in likelihood computation
double max_tobs;

int Nchar;  // number of sites


vector<int> obs_num_wgd;  // possible number of WGD events
vector<vector<int>> obs_change_chr;
vector<int> sample_max_cn;

map<int, set<vector<int>>> decomp_table;  // possible state combinations for observed copy numbers
set<vector<int>> comps;

LNL_TYPE lnl_type;
OBS_DECOMP obs_decomp;

OPT_TYPE opt_type;

//create a list of nodes to loop over, making sure the root is last
vector<int> knodes;

typedef std::numeric_limits<double> dbl;

// unary function and pointer to unary function
// allows use of gsl rng for standard template algorithms
inline long unsigned myrng(long unsigned n){
  return gsl_rng_uniform_int(r, n);
}

long unsigned (*fp_myrng)(long unsigned);


// Randomly pick a tree to perturb by ordering string representation of the tree
evo_tree perturb_tree_set(vector<evo_tree>& trees, gsl_rng* r, long unsigned (*fp_myrng)(long unsigned)){
    int debug = 0;
    if(debug) cout << "\tperturb a set of trees" << endl;

    int count = 0;
    while(true){
        // randomly sample the fit population
        int ind = gsl_rng_uniform_int(r, trees.size());

        // generate a new tree
        evo_tree ttree = perturb_tree(trees[ind], fp_myrng);

        string tstring = order_tree_string_uniq(create_tree_string_uniq(ttree));
        if(searched_trees.find(tstring) == searched_trees.end()){
          // mark the tree
          searched_trees[tstring] = 0;
          return ttree;
        }
        else{
          //cout << "Tree already present" << endl;
          count++;
        }

        if(count > MAX_TREE){
          //cout << "\tperturb_tree cannot find new topologies" << endl;
          exhausted_tree_search = true;
          return ttree;
        }
    }
}



// Generate initial set of unique trees, at most Npop trees, either reading from files or generating random coalescence trees
// Ne, beta, gtime for generating coalescence tree
vector<evo_tree> get_initial_trees(int init_tree, string dir_itrees, int Npop, const vector<double>& rates, int max_tree_num, int Ne = 1, double beta = 0, double gtime = 1){
    int debug = 0;
    vector<evo_tree> trees;

    if(init_tree){     // read MP trees from files
        assert(dir_itrees != "");
        string fname;
        boost::filesystem::path p(dir_itrees);
        for (auto&& x : boost::filesystem::directory_iterator(p)){
            fname = x.path().string();
            evo_tree rtree = read_parsimony_tree(fname, Ns, rates, tobs);
            string tstring = order_tree_string_uniq(create_tree_string_uniq(rtree));

            if(searched_trees.find(tstring) == searched_trees.end()) {
                searched_trees[tstring] = 0;
            }
            rtree.score = -MAX_NLNL;
            trees.push_back(rtree);
        }
    }else{
        int n = (max_tree_num < Npop) ? max_tree_num: Npop;
        if(debug) cout << "generating " << n << " start trees" << endl;

        int num_tree = 0;
        while(num_tree < n){
            evo_tree rtree = generate_coal_tree(Ns, r, fp_myrng, Ne, beta, gtime);

            // tree branch lengths may violate constaints
            bool wrong_blen = false;
            for(int i = 0; i < rtree.edges.size(); i++){
              edge* e = &rtree.edges[i];
              // skip the normal branch which is always 0
              if(e->start == rtree.nleaf && e->end == rtree.nleaf - 1) continue;
              if(e->length < BLEN_MIN || e->length > BLEN_MAX){
                wrong_blen = true;
                // cout << "branch lengths not in the range!" << endl;
                // rtree.print();
                break;
              }
            } // end for loop
            if(wrong_blen)  continue;

            // string tstring = rtree.make_newick(0);
            // tstring.erase(remove_if(tstring.begin(), tstring.end(), [](char c) { return !(c == '(' || c == ')'); }), tstring.end());
            string tstring = order_tree_string_uniq(create_tree_string_uniq(rtree));

            if(debug){
              cout << "tree " << num_tree << " is " << tstring << endl;
            }

            if(searched_trees.find(tstring) == searched_trees.end()){
                searched_trees[tstring] = 0;
                num_tree += 1;
            }else{
                continue;
            }

            if(cons){
                for(int i = 0; i < rtree.nodes.size(); i++){
                    Node* node = &rtree.nodes[i];
                    if(node->id < Ns){
                      node->time += tobs[i];
                    }
                }
                rtree.calculate_age_from_time();
                // update branch lengths based on node times
                for(int i = 0; i < rtree.edges.size(); i++){
                    edge *e = &rtree.edges[i];
                    if(e->end < Ns) e->length += tobs[e->end];
                }

                if(debug) {
                    cout << "Adjust the initial tree by time constraint" << endl;
                    assert(is_tree_valid(rtree, max_tobs, age, cons));
                }
            }

            restore_mutation_rates(rtree, rates);
            rtree.score = -MAX_NLNL;

            if(debug > 1){
                cout << "inital tree " << num_tree << endl;
                rtree.print();
            }
            trees.push_back( rtree );
        }
    }

    if(debug){
        ofstream out_tree("./inital_trees.txt");
        for(int i = 0; i < trees.size(); ++i){
            int precision = 5;
            string newick = trees[i].make_newick(precision);
            out_tree << newick << endl;
            out_tree << order_tree_string_uniq(create_tree_string_uniq(trees[i])) << endl;
        }
        out_tree.close();
    }

    return trees;
}


// Finding n trees with higher likelihood
// There may be multiple trees with the same likelihood, getting the one with maximum score (log likelihood)
// access issues with pointers exist
vector<evo_tree> find_best_trees(const vector<evo_tree>& trees, const vector<double>& lnLs, vector<int>& index, int n){
    vector<evo_tree> btrees;

    iota(index.begin(), index.end(), 0);
    sort(index.begin(), index.end(), [&](int i, int j){ return lnLs[i] > lnLs[j]; } );

    for(int i = 0; i < n; ++i){
        btrees.push_back(trees[index[i]]);
    }

    return btrees;
}



// Only feasible for trees with fewer than 12 samples
// Do maximization multiple times (determined by Ngen), since numerical optimizations are local hill-climbing algorithms and may converge to a local peak
void do_exhaustive_search(evo_tree& min_nlnl_tree, string real_tstring, int Ngen, const int init_tree, const string& dir_itrees, const int& max_static, const vector<double>& rates, const double ssize, const double tolerance, const int miter, const int optim, int Ne = 1, double beta = 0, double gtime = 1){
    // initialize candidate tree set
    if(Ns > LARGE_TREE){
        cout << "\nFor data with larger than " << LARGE_TREE << " samples, it is very slow!" << endl;
        exit(EXIT_FAILURE);
    }

    int max_tree_num = num_trees[Ns - 1];
    cout << "\nMaximum number of possible trees to explore " << max_tree_num << endl;
    vector<evo_tree> init_trees = get_initial_trees(init_tree, dir_itrees, max_tree_num, rates, max_tree_num, Ne, beta, gtime);

    assert(max_tree_num == init_trees.size());
    cout << "Initial number of trees " << max_tree_num << endl;
    if(debug){
        cout << "\nString for real tree is " << real_tstring << endl;
    }

    vector<double> lnLs(max_tree_num, 0.0);
    vector<int> index(max_tree_num, 0);

    #ifdef _OPENMP
    #pragma omp parallel for
    #endif
    for(int i = 0; i < max_tree_num; ++i){
        string tstring = order_tree_string_uniq(create_tree_string_uniq(init_trees[i]));
        if(debug){
            cout << "\nString for tree " << i+1 << " is " << tstring << endl;
            string newick = init_trees[i].make_newick(PRINT_PRECISION);
            cout << "Newick String for tree " << i+1 << " is " << newick << endl;
        }

        // if(debug) cout << "Maximization for tree " << i << endl;
        evo_tree best_tree;
        double nlnl = MAX_NLNL;   // negative likelihood, to be minimalized
        int count_static = 0;
        double min_nlnl = MAX_NLNL;
        int count = 0;

        while(count < Ngen){
            // if(debug) cout << "Maximization in iteration " << count << endl;
            if(optim == 0){
                max_likelihood(init_trees[i], vobs, tobs, lnl_type, opt_type, nlnl, ssize);
            }else{
                // init_trees[i] has already been the same as best_tree
                max_likelihood_BFGS(init_trees[i], vobs, obs_decomp, comps, lnl_type, opt_type, nlnl);
            }
            count++;
            // cout << nlnl << " \t " << min_nlnl << endl;
            if(nlnl < min_nlnl){   // Find a better tree
                min_nlnl = nlnl;
                lnLs[i] = -nlnl;
                init_trees[i].score = -nlnl;
            }else{
                count_static++;
            }
            if(count_static >= max_static){
                if(debug) cout << "\tstatic likelihood " << lnLs[i] << " for tree " << i << endl;
                break;
            }
        }
        cout.precision(dbl::max_digits10);
        string tid = to_string(i);
        if(tstring == real_tstring){
            tid = tid + "(real)";
        }
        cout << "Score for tree " << tid << " is: " << lnLs[i] << endl;
    }

    // Output best tree in C
    cout << "The number of trees searched is " << searched_trees.size() << endl;

    min_nlnl_tree = (find_best_trees(init_trees, lnLs, index, 1)[0]);

    cout << "FINISHED. MIN -ve logL = " << -lnLs[index[0]] << endl;
    cout << "The best tree reported is tree " << index[0] << endl;
}


// Npop determines the maximum number of unique trees to try
// assume nni5 = true
// have to update knodes when topolgy is changed
void do_hill_climbing(evo_tree& min_nlnl_tree, int Npop, int Ngen, int init_tree, const string& dir_itrees, const vector<double>& rates, double ssize, int optim, int Ne = 1, double beta = 0, double gtime = 1){
    int debug = 0;

    int max_tree_num = INT_MAX;
    if(Ns <= LARGE_TREE)   max_tree_num = num_trees[Ns - 1];

    // initialize candidate tree set
    vector<evo_tree> trees = get_initial_trees(init_tree, dir_itrees, Npop, rates, max_tree_num, Ne, beta, gtime);
    int num2init = trees.size();
    vector<double> lnLs(num2init, 0.0);
    vector<int> index(num2init, 0);      // index of trees starting from 0

    cout << "\tInitial number of trees " << num2init << endl;

    // no topolgy change
    #ifdef _OPENMP
    #pragma omp parallel for
    #endif
    for(int i = 0; i < num2init; ++i){
        double nlnl = MAX_NLNL;

        if(optim == 0){  // use gsl libaries (deprecated)
            while(!(nlnl < MAX_NLNL)){
                nlnl = MAX_NLNL;
                max_likelihood(trees[i], vobs, tobs, lnl_type, opt_type, nlnl, ssize);
            }
        }else{
            while(!(nlnl < MAX_NLNL)){
                nlnl = MAX_NLNL;
                max_likelihood_BFGS(trees[i], vobs, obs_decomp, comps, lnl_type, opt_type, nlnl);
            }
        }
        trees[i].score = -nlnl;
        lnLs[i] = -nlnl;

        if(debug){
          cout << "likelihood for tree " << i << " is " << -nlnl << endl;
          trees[i].print();
        }
    }

    // Select top MAX_TREE trees for hill climbing NNI to obtain locally optimal ML trees
    int num2perturb = (trees.size() < MAX_TREE1) ? trees.size() : MAX_TREE1;
    vector<evo_tree> trees2 = find_best_trees(trees, lnLs, index, num2perturb);
    vector<double> lnLs2(num2perturb, 0.0);
    vector<int> index2(num2perturb, 0);

    cout << "\tNumber of trees to perturb for hill climbing NNIs " << num2perturb << endl;

    // Perturb trees randomly (NNI may be disturbed by openmp due to use of pointers)
    // #ifdef _OPENMP
    // #pragma omp parallel for
    // #endif
    for(int i = 0; i < num2perturb; ++i){
        trees2[i].generate_neighbors();
        do_hill_climbing_NNI(trees2[i], vobs, obs_decomp, comps, lnl_type, opt_type, loglh_epsilon, speed_nni);
        trees2[i].delete_neighbors();

        lnLs2[i] = trees2[i].score;
    }

    // Keep top 5 trees for further optimization to escape from local optima
    int num2refine = (trees2.size() < MAX_TREE2) ? trees2.size(): MAX_TREE2;
    vector<evo_tree> trees3 = find_best_trees(trees2, lnLs2, index2, num2refine);
    vector<double> lnLs3(num2refine, 0.0);
    vector<int> index3(num2refine, 0);
    for(int i = 0; i < num2refine; ++i){
        lnLs3[i] = trees3[i].score;
    }

    cout << "\tNumber of trees to refine with stochastic and hill climbing NNIs " << num2refine << endl;

    int count = 0;
    // Perturb trees randomly
    while(count < MAX_PERTURB){
        int i = gsl_rng_uniform_int(r, num2refine);
        count += 1;

        if(debug) cout << "\t\tPerturb tree " << i << endl;

        // Randomly perturb a tree to allow escape from local optima, doing 0.5(n-2) random NNIs
        // create a new tree to avoid disrupting original tree
        evo_tree ttree(trees3[i]);   // local, so use trees3 for accessing outside while loop
        ttree.generate_neighbors();

        do_random_NNIs(ttree, r, cons);
        vector<int> inodes;
        Node* root = &(ttree.nodes[ttree.root_node_id]);
        ttree.get_inodes_postorder(root, inodes);
        lnl_type.knodes = inodes;

        do_hill_climbing_NNI(ttree, vobs, obs_decomp, comps, lnl_type, opt_type, loglh_epsilon, speed_nni);
        ttree.delete_neighbors();

        evo_tree btree = find_best_trees(trees3, lnLs3, index3, 1)[0];
        assert(btree.score == lnLs3[index3[0]]);

        double max_lnl = lnLs3[index3[0]];
        double min_lnl = lnLs3[index3[index3.size() - 1]];

        if(ttree.score > max_lnl){  // better than best tree in C
            trees3[index3[index3.size() - 1]] = ttree;
            lnLs3[index3[index3.size() - 1]] = ttree.score;
            count = 0;
        }else{
            // Replace the worst tree
            if(ttree.score > min_lnl){ // better than worst tree in C
                trees3[index3[index3.size() - 1]] = ttree;
                lnLs3[index3[index3.size() - 1]] = ttree.score;
            }
        }
    }

    // Output best tree in C
    cout << "\tThe number of trees searched is " << searched_trees.size() << endl;
    min_nlnl_tree = (find_best_trees(trees3, lnLs3, index3, 1)[0]);

    if(cons && (!is_tip_age_valid(min_nlnl_tree.get_node_ages(), tobs) || !is_age_time_consistent(min_nlnl_tree.get_node_times(), min_nlnl_tree.get_node_ages()))){
      cout << "Wrong tip age or node times!" << endl;
      string newick = min_nlnl_tree.make_newick(PRINT_PRECISION);
      cout << newick << endl;
    }

    cout << "\nFINISHED. MIN -ve logL = " << -min_nlnl_tree.score << endl;

    // print out searched trees
    if(debug){
        min_nlnl_tree.print();
        ofstream out_tree("./searched_trees.txt");
        for(auto it : searched_trees){
            out_tree << it.first << endl;
        }
        out_tree.close();
    }
}


// Using genetic algorithm to search tree space (TODO: optimize)
void do_evolutionary_algorithm(evo_tree& min_nlnl_tree, const int& Npop, const int& Ngen, const int init_tree, const string& dir_itrees, const int& max_static, const vector<double>& rates, const double ssize, const double tolerance, const int miter, const int optim, int Ne = 1, double beta = 0, double gtime = 1){
  //cout << "Running evolutionary algorithm" << endl;
  // create initial population of trees. Sample from coalescent trees
  vector<evo_tree> trees = get_initial_trees(init_tree, dir_itrees, Npop, rates, Npop, Ne, beta, gtime);
  vector<double> lnLs(2 * Npop, 0);
  double min_nlnl = MAX_NLNL;
  double nlnl = 0;
  int count_static = 0;

  for(int g = 0; g < Ngen; ++g){
    // Growth stage: create Npop copies + Npop copies with mutation (topolgy changes)
    // The top scoring trees have already been scored and optimised
    vector<evo_tree> new_trees;
    vector<evo_tree> opt_trees;

    if( g == 0 ){
      for(int i = 0; i < Npop; ++i){
	         new_trees.push_back(trees[i]);
      }
      for(int i = 0; i < Npop; ++i){
          evo_tree new_tree = perturb_tree_set(trees, r, fp_myrng);
          new_tree.score = get_likelihood_revised(new_tree, vobs, lnl_type);
    	    new_trees.push_back(new_tree);
      }

      // Selection: score all the trees in new_trees (all got maximized)
      for(int i = 0; i < 2 * Npop; ++i){
        if(optim == 0){
           max_likelihood(new_trees[i], vobs, tobs, lnl_type, opt_type, nlnl, ssize);
        }
        if(optim == 1){
           max_likelihood_BFGS(new_trees[i], vobs, obs_decomp, comps, lnl_type, opt_type, nlnl);
        }
        new_trees[i].score = -nlnl;
        // cout << "otree tobs " << otree.tobs[0] << endl;
        lnLs[i] = -nlnl;

        string tstring = order_tree_string_uniq(create_tree_string_uniq(new_trees[i]));
        searched_trees[ tstring ] += 1;

        opt_trees.push_back(new_trees[i]);
      }
    }else{
          // Leave this subpopulation unchanged
          for(int i = 0; i < Npop; ++i){
          	new_trees.push_back(trees[i]);
          	opt_trees.push_back(trees[i]);
          	lnLs[i] = new_trees[i].score;
          }

          // cout << "Size of new_trees " << new_trees.size() << endl;
          // cout << "Size of opt_trees " << new_trees.size() << endl;

          // Perturb this subpopulation
          for(int i = 0; i < Npop; ++i){
            evo_tree new_tree = perturb_tree_set(trees, r, fp_myrng);
            new_tree.score = get_likelihood_revised(new_tree, vobs, lnl_type);
      	    new_trees.push_back(new_tree);
            // new_trees of size 2 Npop
            if(optim == 0){
        	    max_likelihood(new_trees[Npop + i], vobs, tobs, lnl_type, opt_type, nlnl, ssize);
            }
            if(optim == 1){
                // new_trees[Npop + i].print();
                max_likelihood_BFGS(new_trees[Npop + i], vobs, obs_decomp, comps, lnl_type, opt_type, nlnl);
            }
        	new_trees[Npop + i].score = -nlnl;
            // cout << "otree tobs " << otree.tobs[0] << endl;
        	lnLs[Npop + i] = -nlnl;

            string tstring = order_tree_string_uniq(create_tree_string_uniq( new_trees[Npop + i] ) );
            searched_trees[tstring] += 1;

        	opt_trees.push_back( new_trees[Npop + i] );
        }
      // Randomly sample again?
    }

    vector<int> index(2 * Npop);
    int x=0;
    iota(index.begin(), index.end(), x++);
    sort(index.begin(), index.end(), [&](int i,int j){return lnLs[i]>lnLs[j];});

    // Selection: calculate mean fitness of top half of population
    double meand = 0;
    for(int k=0; k < Npop; ++k){
      //cout << "\t\t" << lnLs[ index[k] ] << endl;
      meand += lnLs[index[k]];
    }
    meand = meand / Npop;
    if(g%1 == 0) cout << "g / av dist / top dist / trees searched \t" << g << "\t" << meand << "\t" << lnLs[ index[0] ] << "\t" << searched_trees.size() << endl;

    // Selection: select top half
    for(int i = 0; i < Npop; ++i){
      trees[i] = opt_trees[ index[i] ];
    }

    // Selection: record the best (lowest) scoring tree
    if(-lnLs[ index[0] ] < min_nlnl){
      min_nlnl = -lnLs[ index[0] ];
      min_nlnl_tree = opt_trees[ index[0] ];
      count_static = 0;
      min_nlnl_tree.print();
    }else{
      cout << "min static" << endl;
      count_static += 1;
    }

    if(max_static > 0 && count_static == max_static){
      cout << "\t### static likelihood function. Finishing on ngen = " << g << endl;
      break;
    }

    fill(lnLs.begin(), lnLs.end(), 0);

    // A tree has been maximized at least five times
    int sum_max_num = 0;
    for(auto it : searched_trees){
        sum_max_num += it.second;
    }
    // cout << "Total times of maximization " << sum_max_num << endl;

    if( exhausted_tree_search == true && sum_max_num > MAX_OPT * searched_trees.size()){
      cout << "\tperturb_tree struggling to find new topologies. Either exhausted possible trees or local minimum" << endl;
      break;
    }
  }

  cout << "FINISHED. MIN -ve logL = " << min_nlnl << endl;

  // print out searched trees
  // ofstream out_tree("./test/searched_trees.txt");
  // for(auto it : searched_trees){
  //     out_tree << it.first << endl;
  // }
  // out_tree.close();
}



// Get the mutation rates in year along each branch
// num_total_bins: number of total segments in the (haploid) genome
// unit of mutation rate: duplication/deletion rate -- bin/allele/year, chromosome gain/loss rate -- chromosome/year, WGD date -- year
vector<int> compute_mutation_rates(evo_tree& tree, int only_seg, int num_total_bins){
    double mu_est;  // mutation rate per year
    vector<double> mu_all;
    vector<int> nmuts;

    if(!only_seg){
        mu_est = NORM_PLOIDY * NUM_CHR * (tree.chr_gain_rate + tree.chr_loss_rate) + NORM_PLOIDY * num_total_bins * (tree.dup_rate + tree.del_rate) + tree.wgd_rate;
    }
    else{
        mu_est = NORM_PLOIDY * num_total_bins * (tree.dup_rate + tree.del_rate);
    }

    mu_all.clear();
    for(int i = 0; i < tree.edges.size(); i++){
        mu_all.push_back(mu_est);
    }

    nmuts = tree.get_nmuts(mu_all);

    return nmuts;
}



// Run the program on a given tree with different modes of estimation (branch length constrained or not, mutation rate estimated or not)
void run_test(const string& tree_file, int Ns, int num_total_bins, int Nchar, int model, int cn_max, int only_seg, int correct_bias, int is_total, const vector<double>& tobs, const vector<vector<int>>& vobs0, int Nchar0, const vector<double>& rates, double ssize, double tolerance, double miter){
    // MLE testing
    //static const int arr1[] = {8,5, 8,1, 9,2, 9,3, 10,9, 10,8, 11,4, 11,10, 7,11, 7,6 };
    //vector<int> e (arr1, arr1 + sizeof(arr1) / sizeof(arr1[0]) );
    //for(int i = 0; i < e.size();++i) e[i] = e[i] - 1;
    //static const double arr2[] = {18.49, 38.49, 51.71, 31.71, 0.51, 3.73, 22.2, 0.013, 0.99, 0};
    //vector<double> l (arr2, arr2 + sizeof(arr2) / sizeof(arr2[0]) );

    // read in true tree
    evo_tree test_tree = read_tree_info(tree_file, Ns);
    restore_mutation_rates(test_tree, rates);

    double Ls = 0.0;
    double nlnl = 0;  // Used in maximization
    string fname = "";
    ofstream out_tree;
    evo_tree min_tree;
    double mu_est;
    vector<double> mu_all;
    vector<int> nmuts;

    cout << "Computing likelihood of the given tree" << endl;
    Ls = get_likelihood(vobs0, test_tree, knodes, model, cn_max, is_total);
    cout << "\nOriginal tree -ve likelihood with original method: " << -Ls << endl;

    Ls = get_likelihood_revised(test_tree, vobs, lnl_type);
    cout << "\nOriginal tree -ve likelihood with revised method: " << -Ls << endl;

    cout << "\n\n### Running optimisation: branches free, mu fixed" << endl;
    // min_tree = max_likelihood(test_tree, model, nlnl, ssize, tolerance, miter, 0, 0, cn_max, only_seg, correct_bias);
    // cout << "\nMinimised tree likelihood / mu : " << nlnl << "\t" << min_tree.mu <<  endl;
    // min_tree.print();
    // fname = tree_file + ".opt00_gsl.txt";
    // out_tree.open(fname);
    // min_tree.write(out_tree);
    // out_tree.close();
    nlnl = 0;
    cons = 0;
    maxj = 0;
    max_likelihood_BFGS(test_tree, vobs, obs_decomp, comps, lnl_type, opt_type, nlnl);
    min_tree = test_tree;
    cout << "\nMinimised tree likelihood / mu by BFGS : " << nlnl << "\t" << min_tree.dup_rate << "\t" << min_tree.del_rate;
    if(!only_seg){
        cout << "\t" << min_tree.chr_gain_rate << "\t" << min_tree.chr_loss_rate << "\t" << min_tree.wgd_rate;
    }
    cout << endl;
    min_tree.print();
    fname = tree_file + ".opt00_bfgs.txt";
    out_tree.open(fname);
    min_tree.write_with_mut(out_tree, compute_mutation_rates(min_tree, only_seg, num_total_bins));
    out_tree.close();


    cout << "\n\n### Running optimisation: branches free, mu free" << endl;
    // nlnl = 0;
    // min_tree = max_likelihood(test_tree, model, nlnl, ssize, tolerance, miter, 0, 1, cn_max, only_seg, correct_bias);
    // cout << "\nMinimised tree likelihood / mu : " << nlnl << "\t" << min_tree.mu << endl;
    // min_tree.print();
    // fname = tree_file + ".opt01_gsl.txt";
    // out_tree.open(fname);
    // min_tree.write(out_tree);
    // out_tree.close();
    test_tree = read_tree_info(tree_file, Ns);
    restore_mutation_rates(test_tree, rates);
    nlnl = 0;
    cons = 0;
    maxj = 1;
    max_likelihood_BFGS(test_tree, vobs, obs_decomp, comps, lnl_type, opt_type, nlnl);
    min_tree = test_tree;
    cout << "\nMinimised tree likelihood / mu by BFGS : " << nlnl << "\t" << min_tree.dup_rate << "\t" << min_tree.del_rate;
    if(!only_seg){
        cout << "\t" << min_tree.chr_gain_rate << "\t" << min_tree.chr_loss_rate << "\t" << min_tree.wgd_rate;
    }
    cout << endl;
    min_tree.print();
    fname = tree_file + ".opt01_bfgs.txt";
    out_tree.open(fname);
    // min_tree.write(out_tree
    min_tree.write_with_mut(out_tree, compute_mutation_rates(min_tree, only_seg, num_total_bins));
    out_tree.close();


    cout << "\n\n### Running optimisation: branches constrained, mu fixed" << endl;
    // nlnl = 0;
    // min_tree = max_likelihood(test_tree, model, nlnl, ssize, tolerance, miter, 1, 0, cn_max, only_seg, correct_bias);
    // cout << "\nMinimised tree likelihood / mu : " << nlnl << "\t" << min_tree.mu <<endl;
    // min_tree.print();
    // fname = tree_file + ".opt10_gsl.txt";
    // out_tree.open(fname);
    // min_tree.write(out_tree);
    // out_tree.close();
    test_tree = read_tree_info(tree_file, Ns);
    restore_mutation_rates(test_tree, rates);
    nlnl = 0;
    cons = 1;
    maxj = 0;
    max_likelihood_BFGS(test_tree, vobs, obs_decomp, comps, lnl_type, opt_type, nlnl);
    min_tree = test_tree;
    cout << "\nMinimised tree likelihood / mu by BFGS: " << nlnl << "\t" << min_tree.dup_rate << "\t" << min_tree.del_rate;
    if(!only_seg){
        cout << "\t" << min_tree.chr_gain_rate << "\t" << min_tree.chr_loss_rate << "\t" << min_tree.wgd_rate;
    }
    cout << endl;
    min_tree.print();
    fname = tree_file + ".opt10_bfgs.txt";
    out_tree.open(fname);
    // min_tree.write(out_tree);
    min_tree.write_with_mut(out_tree, compute_mutation_rates(min_tree, only_seg, num_total_bins));
    out_tree.close();


    cout << "\n\n### Running optimisation: branches constrained, mu free" << endl;
    // nlnl = 0;
    // min_tree = max_likelihood(test_tree, model, nlnl, ssize, tolerance, miter, 1, 1, cn_max, only_seg, correct_bias);
    // cout << "\nMinimised tree likelihood / mu : " << nlnl << "\t" << min_tree.mu*Nchar <<endl;
    // min_tree.print();
    // fname = tree_file + ".opt11_gsl.txt";
    // out_tree.open(fname);
    // min_tree.write(out_tree);
    // out_tree.close();
    test_tree = read_tree_info(tree_file, Ns);
    restore_mutation_rates(test_tree, rates);
    nlnl = 0;
    cons = 1;
    maxj = 1;
    max_likelihood_BFGS(test_tree, vobs, obs_decomp, comps, lnl_type, opt_type, nlnl);
    min_tree = test_tree;
    cout << "\nMinimised tree likelihood / mu by BFGS : " << nlnl << "\t" << min_tree.dup_rate << "\t" << min_tree.del_rate;
    if(!only_seg){
        cout << "\t" << min_tree.chr_gain_rate << "\t" << min_tree.chr_loss_rate << "\t" << min_tree.wgd_rate;
    }
    cout << endl;
    min_tree.print();
    fname = tree_file + ".opt11_bfgs.txt";
    out_tree.open(fname);
    // min_tree.write(out_tree);
    min_tree.write_with_mut(out_tree, compute_mutation_rates(min_tree, only_seg, num_total_bins));
    out_tree.close();

}


// Compute the likelihood of a tree given the observed copy number profile
double compute_tree_likelihood(const string& tree_file, map<int, set<vector<int>>>& decomp_table, int Ns, int Nchar, int num_invar_bins, map<int, vector<vector<int>>>& vobs, vector<double>& tobs, vector<double>& rates, int model, int cons, int cn_max, int max_wgd, int max_chr_change, int max_site_change, int only_seg, int correct_bias, int is_total=1){
    evo_tree tree;
    // string format = tree_file.substr(tree_file.find_last_of(".") + 1);
    // cout << "Format of the input tree file is " << format << endl;

    // if(format == "txt"){
    tree = read_tree_info(tree_file, Ns);
    restore_mutation_rates(tree, rates);
    // }
    // if(format == "nex"){
    //     tree = read_nexus(tree_file);
    // }

    cout << "Node times: ";
    for(int i = 0; i < tree.nodes.size(); i++){
        cout << "\t" << tree.nodes[i].time;
    }
    cout<< endl;

    // int sample1 = 0;
    // tree.get_ntime_interval(Ns, sample1);
    // cout << "Top " << Ns << " time intervals: ";
    // for(int i = 0; i < tree.top_tinvls.size(); i++){
    //     cout << "\t" << tree.top_tinvls[i];
    // }
    // cout<< endl;
    // cout << "Corresponding " << Ns + 1 << " nodes: ";
    // for(int i = 0; i < tree.top_tnodes.size(); i++){
    //     cout << tree.top_tnodes[i] + 1 << "\t" << "\t" << tree.node_times[tree.top_tnodes[i]] << endl;
    // }
    //
    // vector<double> blens = tree.get_edges_from_interval(tree.top_tinvls, tree.top_tnodes);
    // cout << "Branch lengths computed from intervals: ";
    // for(int i = 0; i < blens.size(); i++){
    //     cout << i + 1 << "\t" << "\t" << blens[i] << endl;
    // }

    double Ls = 0;


    if(model == DECOMP){
      cout << "\nComputing the likelihood based on independent Markov chain model " << endl;
      Ls = get_likelihood_decomp(tree, vobs, obs_decomp, comps, lnl_type);
    }else{
      cout << "\nComputing the likelihood based on allele-specific model " << endl;
      Ls = get_likelihood_revised(tree, vobs, lnl_type);
    }


    return Ls;
}


// Given a tree, compute its maximum likelihood
void maximize_tree_likelihood(const string& tree_file, const string& ofile, int Ns, int Nchar, vector<double>& tobs, const vector<double>& rates, double ssize, double tolerance, int miter, int model, int optim, int cons, int maxj, int cn_max, int only_seg, int correct_bias, int is_total){
    evo_tree tree;
    // string format = tree_file.substr(tree_file.find_last_of(".") + 1);
    // cout << "Format of the input tree file is " << format << endl;

    // if(format == "txt"){
    tree = read_tree_info(tree_file, Ns);
    tree.print();
    // }
    // if(format == "nex"){
    //     tree = read_nexus(tree_file);
    // }
    // if(cons){
    //     adjust_tree_blens(tree);
    //     adjust_tree_height(tree);
    //     adjust_tree_tips(tree);
    // }

    restore_mutation_rates(tree, rates);

    double nlnl = 0.0;

    if(optim == 1){
        max_likelihood_BFGS(tree, vobs, obs_decomp, comps, lnl_type, opt_type, nlnl);
    }else{
        max_likelihood(tree, vobs, tobs, lnl_type, opt_type, nlnl, ssize);
    }
    cout << "\nMinimised tree likelihood: " << nlnl << endl;
    if(maxj){
        cout << "Estimated mutation rates: " << endl;
        cout << "\t" << tree.dup_rate << "\t" << tree.del_rate;
        if(!only_seg){
            cout << "\t" << tree.chr_gain_rate << "\t" << tree.chr_loss_rate << "\t" << tree.wgd_rate;
        }
        cout << endl;
    }
    tree.print();

    ofstream out_tree(ofile);
    tree.write(out_tree);
    out_tree.close();
}


// Build ML tree from given CNPs
void find_ML_tree(string real_tstring, int total_chr, int num_total_bins, string ofile, int tree_search, int Npop, int Ngen, int init_tree, string dir_itrees, int max_static, double ssize, double tolerance, int miter, int optim, const vector<double>& rates, int Ne = 1, double beta = 0, double gtime=1){
    int debug = 0;

    evo_tree min_nlnl_tree;
    if(tree_search == 0){
        cout << "\nSearching tree space with evolutionary algorithm" << endl;
        do_evolutionary_algorithm(min_nlnl_tree, Npop, Ngen, init_tree, dir_itrees, max_static, rates, ssize, tolerance, miter, optim, Ne, beta, gtime);
    }else if(tree_search == 1){
        cout << "\nSearching tree space with hill climbing algorithm" << endl;
        assert(Ns > 4);
        do_hill_climbing(min_nlnl_tree, Npop, Ngen, init_tree, dir_itrees, rates, ssize, optim, Ne, beta, gtime);
    }else{
        cout << "\nSearching tree space exhaustively (only feasible for small trees)" << endl;
        // cout << "Parameters: " << Ngen << "\t" << Ns << "\t" << Nchar << "\t" << num_invar_bins << "\t" << model << "\t" << cons << "\t" << cn_max << "\t" << only_seg << "\t" << correct_bias << "\t" << is_total << endl;
        do_exhaustive_search(min_nlnl_tree, real_tstring, Ngen, init_tree, dir_itrees, max_static, rates, ssize, tolerance, miter, optim, Ne, beta, gtime);
    }

    if(debug) cout << "Writing results ......" << endl;
    // Write out the top tree
    cout.precision(PRINT_PRECISION);
    min_nlnl_tree.print();

    if(maxj){
        if(model == MK){
          cout << "Estimated mutation rate:  " << min_nlnl_tree.mu << endl;
        }else{
          cout << "Estimated duplication rate:  " << min_nlnl_tree.dup_rate << endl;
          cout << "Estimated deletion rate:  " << min_nlnl_tree.del_rate << endl;
          if(!only_seg){
              cout << "Estimated chromosome gain rate (year):  " << min_nlnl_tree.chr_gain_rate << endl;
              cout << "Estimated chromosome loss rate (year):  " << min_nlnl_tree.chr_loss_rate << endl;
              cout << "Estimated whole genome doubling rate (year):  " << min_nlnl_tree.wgd_rate << endl;
          }
        }
    }else{
      if(model == MK){
        cout << "Pre-specified mutation rate:  " << min_nlnl_tree.mu << endl;
      }else{
        cout << "Pre-specified duplication rate:  " << min_nlnl_tree.dup_rate << endl;
        cout << "Pre-specified deletion rate:  " << min_nlnl_tree.del_rate << endl;
        if(!only_seg){
            cout << "Pre-specified chromosome gain rate (year):  " << min_nlnl_tree.chr_gain_rate << endl;
            cout << "Pre-specified chromosome loss rate (year):  " << min_nlnl_tree.chr_loss_rate << endl;
            cout << "Pre-specified whole genome doubling rate (year):  " << min_nlnl_tree.wgd_rate << endl;
        }
      }
    }

    // Recompute the likelihood to check it is not affected by tree adjustment
    if(debug > 0){
        double lnL = 0.0;
        if(model == DECOMP){
            lnL = get_likelihood_decomp(min_nlnl_tree, vobs, obs_decomp, comps, lnl_type);
        }else{
            lnL = get_likelihood_revised(min_nlnl_tree, vobs, lnl_type);
        }
        cout.precision(dbl::max_digits10);
        cout << "Recomputed log likelihood " << lnL << endl;
    }


    // Check the validity of the tree
    if(cons && !is_tree_valid(min_nlnl_tree, max_tobs, age, cons)){
      cout << "The final tree is not valid!" << endl;
    }

    // When mutation rates are not estimated, using specified rates to compute number of mutations
    double mu_est = 0.0;
    if(!only_seg){
        mu_est = NORM_PLOIDY * total_chr * (min_nlnl_tree.chr_gain_rate + min_nlnl_tree.chr_loss_rate) + NORM_PLOIDY * num_total_bins * (min_nlnl_tree.dup_rate + min_nlnl_tree.del_rate) + min_nlnl_tree.wgd_rate;
    }else{
        mu_est = NORM_PLOIDY * num_total_bins * (min_nlnl_tree.dup_rate + min_nlnl_tree.del_rate);
    }

    cout << "Total mutation rate per year " << mu_est << endl;
    vector<double> mu_all;
    for(int i = 0; i < min_nlnl_tree.edges.size(); i++){
        mu_all.push_back(mu_est);
    }
    vector<int> nmuts = min_nlnl_tree.get_nmuts(mu_all);

    ofstream out_tree(ofile);
    min_nlnl_tree.write_with_mut(out_tree, nmuts);
    out_tree.close();

    // branch length as time, not meaningful when pre-specified mutation rate in inaccurate
    string ofile_nex = ofile + ".nex";
    ofstream nex_tree(ofile_nex);
    int precision = 5;
    string newick = min_nlnl_tree.make_newick(precision);
    min_nlnl_tree.write_nexus(newick, nex_tree);
    nex_tree.close();

    // branch length as number of mutations
    ofile_nex =  ofile + ".nmut.nex";
    ofstream nex_tree2(ofile_nex);
    precision = 0;
    newick = min_nlnl_tree.make_newick_nmut(precision, nmuts);
    min_nlnl_tree.write_nexus(newick, nex_tree2);
    nex_tree2.close();
}



int main(int argc, char** const argv){
    int Npop, Ngen, Ne;
    int max_static, bootstrap, optim, mode, tree_search, init_tree;
    double ssize;
    double mu, dup_rate, del_rate, chr_gain_rate, chr_loss_rate, wgd_rate, max_rate;
    double beta, gtime;
    string datafile, timefile, ofile, tree_file, dir_itrees;
    int is_bin, incl_all;
    int infer_marginal_state, infer_joint_state;
    double min_asr;

    namespace po = boost::program_options;
    po::options_description generic("Generic options");
    generic.add_options()
    ("version,v", "print version string")
    ("help,h", "produce help message")
    ;
    po::options_description required("Required parameters");
    required.add_options()
     ("cfile,c", po::value<string>(&datafile)->required(), "input copy number profile file")
     ;
    po::options_description optional("Optional parameters");
    optional.add_options()
     ("tfile,t", po::value<string>(&timefile)->default_value(""), "input time information file")

    ("nsample,s", po::value<int>(&Ns)->default_value(5), "number of samples or regions")
    ("ofile,o", po::value<string>(&ofile)->default_value("maxL-tree.txt"), "output tree file with maximum likelihood")
    // ("ofile_nex,o", po::value<string>(&ofile_nex)->default_value("maxL-tree.nex"), "output tree file with maximum likelihood in NEXUS format")

    ("tree_file", po::value<string>(&tree_file)->default_value(""), "input tree file")

    ("mode", po::value<int>(&mode)->default_value(1), "running mode of the program (0: Compute maximum likelihood tree from copy number profile; 1: Test on example data; 2: Compute the likelihood of a given tree with branch length; 3: Compute the maximum likelihood of a given tree)")
    ("bootstrap,b", po::value<int>(&bootstrap)->default_value(0), "doing bootstrap or not")

    ("model,d", po::value<int>(&model)->default_value(2), "model of evolution (0: Mk, 1: one-step bounded (total), 2: one-step bounded (allele-specific, 3: independent Markov chains)")
    ("constrained", po::value<int>(&cons)->default_value(1), "constraints on branch length (0: none, 1: fixed total time)")
    ("estmu,u", po::value<int>(&maxj)->default_value(0), "estimation of mutation rate (0: mutation rate fixed to be the given value, 1: estimating mutation rate)")
    ("optim", po::value<int>(&optim)->default_value(1), "method of optimization (0: Simplex, 1: L-BFGS-B)")

    // limits on copy number changes
    ("cn_max", po::value<int>(&cn_max)->default_value(4), "maximum copy number of a segment")
    ("m_max", po::value<int>(&m_max)->default_value(1), "maximum number of copies of a segment in a chromosome")
    ("max_wgd", po::value<int>(&max_wgd)->default_value(1), "maximum number of WGD")
    ("max_chr_change", po::value<int>(&max_chr_change)->default_value(1), "maximum number of chromosome changes")
    ("max_site_change", po::value<int>(&max_site_change)->default_value(2), "maximum number of segment changes")

    // types of copy number changes
    ("is_total", po::value<int>(&is_total)->default_value(1), "whether or not the input is total copy number")
    ("is_bin", po::value<int>(&is_bin)->default_value(1), "whether or not the input copy number is for each bin. If not, the input copy number is read as it is. Or else, consecutive bins will be merged")
    ("incl_all", po::value<int>(&incl_all)->default_value(1), "whether or not to include all the input copy numbers without further propressing")

    ("only_seg", po::value<int>(&only_seg)->default_value(0), "Whether or not to only consider segment-level mutations (0: include chromosome gain/loss and whole genome doubling, 1: only consider segment-level mutations)")
    ("infer_wgd", po::value<int>(&infer_wgd)->default_value(0), "whether or not to infer WGD status of a sample from its copy numbers")
    ("infer_chr", po::value<int>(&infer_chr)->default_value(0), "whether or not to infer chromosome gain/loss status of a sample from its copy numbers")

    ("use_repeat", po::value<int>(&use_repeat)->default_value(1), "whether or not to use repeated site patterns when computing the likelihood")
    ("correct_bias", po::value<int>(&correct_bias)->default_value(1), "correct ascertainment bias")

    // options related to inferring ancestry state
    ("infer_marginal_state", po::value<int>(&infer_marginal_state)->default_value(1), "whether or not to infer marginal ancestral state of MRCA")
    ("infer_joint_state", po::value<int>(&infer_joint_state)->default_value(1), "whether or not to infer joint ancestral state of all internal nodes")
    ("min_asr", po::value<double>(&min_asr)->default_value(0.5), "minimum posterior probability to determine the best ancestral state")

    ("init_tree", po::value<int>(&init_tree)->default_value(0), "method of building inital tree (0: Random coalescence tree, 1: Maximum parsimony tree)")
    ("dir_itrees", po::value<string>(&dir_itrees)->default_value(""), "directory containing provided inital trees")

    // options related to simulation of random coalescent tree
    ("epop", po::value<int>(&Ne)->default_value(2), "effective population size of cell populations")
    ("gtime", po::value<double>(&gtime)->default_value(1), "generation time in year")
    ("beta", po::value<double>(&beta)->default_value(0), "population growth rate")

    // options related to tree searching
    ("tree_search", po::value<int>(&tree_search)->default_value(1), "method of searching tree space (0: Genetic algorithm, 1: Random-restart hill climbing, 2: Exhaustive search)")
    ("speed_nni", po::value<int>(&speed_nni)->default_value(1), "whether or not to do reduced NNI while doing hill climbing NNIs")
    ("npop,p", po::value<int>(&Npop)->default_value(100), "number of population in genetic algorithm or maximum number of initial trees")
    ("ngen,g", po::value<int>(&Ngen)->default_value(50), "number of generation in genetic algorithm or maximum number of times to perturb/optimize a tree")
    ("nstop,e", po::value<int>(&max_static)->default_value(10), "Stop after this number of times that the tree does not get improved")
    ("tolerance,r", po::value<double>(&tolerance)->default_value(1e-2), "tolerance value in maximization methods")
    ("miter,m", po::value<int>(&miter)->default_value(2000), "maximum number of iterations in maximization")
    ("loglh_epsilon", po::value<double>(&loglh_epsilon)->default_value(0.001), "tolerance value bewteen log likelihood values")
    ("ssize,z", po::value<double>(&ssize)->default_value(0.01), "initial step size used in GSL optimization")

    // mutation rates
    ("mu,x", po::value<double>(&mu)->default_value(0.02), "overall mutation rate")
    ("dup_rate", po::value<double>(&dup_rate)->default_value(0.01), "duplication rate")
    ("del_rate", po::value<double>(&del_rate)->default_value(0.01), "deletion rate")
    ("chr_gain_rate", po::value<double>(&chr_gain_rate)->default_value(0), "chromosome gain rate")
    ("chr_loss_rate", po::value<double>(&chr_loss_rate)->default_value(0), "chromosome loss rate")
    ("wgd_rate", po::value<double>(&wgd_rate)->default_value(0), "WGD (whole genome doubling) rate")

    ("verbose", po::value<int>(&debug)->default_value(0), "verbose level (0: default, 1: debug)")
    ("seed", po::value<unsigned>(&seed)->default_value(0), "seed used for generating random numbers")
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
          cout << "svtreeml [version 0.1], a program to build a maximum likelihood phylogenetic tree from copy number profile" << endl;
          cout << "This program can also be used to compute the likelihood of a tree with fixed branch lengths or maximum likelihood of a tree with fixed topology (and/or mutation rates), given the copy number profile." << endl;
          return 1;
      }
      po::notify(vm);
    } catch (const std::exception& e) {
        std::cerr << e.what() << std::endl;
        return 1;
    }

    assert(optim <= 1);
    assert(model <= 3);

    gsl_rng_env_setup();
    const gsl_rng_type* T = gsl_rng_default;
    r = gsl_rng_alloc(T);
    setup_rng(r, seed);

    fp_myrng = &myrng;

    // cout << fp_myrng(10) << endl;

    if(!cons){
      cout << "\nAssuming the tree is unconstrained when doing optimization" << endl;
    }else{
      cout << "\nAssuming the tree is constrained by age at sampling time" << endl;
    }

    if(!maxj){
      cout << "\nAssuming mutation rate is fixed " << endl;
    }else{
      cout << "\nEstimating mutation rates" << endl;
      if(!only_seg){
        cout << "\tfor segment duplication/deletion, chromosome gain/loss, and whole genome doubling " << endl;
      }else{
        cout << "\tfor segment duplication/deletion " << endl;
      }
    }

    if(!correct_bias){
      cout << "\nNot correcting acquisition bias in likelihood computation " << endl;
    }else{
      cout << "\nCorrecting acquisition bias in likelihood computation " << endl;
    }

    if(optim == 0){
      cout << "\nUsing Simplex method for optimization" << endl;
    }else{
      cout << "\nUsing L-BFGS-B method for optimization" << endl;
    }

    // tobs already defined globally
    if(timefile != ""){
        tobs = read_time_info(timefile, Ns, age);
    }else{
        tobs = vector<double>(Ns, 0.0);
    }
    cout << "\nInput real sample times:";
    for(auto t : tobs){
      cout << "\t" << t;
    }
    cout << endl;

    if(cons){
        cout << "\nThe age of patient at the first sampling time: " << age << endl;
    }else{
        age = MAX_AGE;
        cout << "\nThe age of patient is assumed to be: " << age << endl;
    }

    switch(model){
        case MK: {
            cout << "\nAssuming Mk model " << endl;
            break;
        }
        case BOUNDT:{
            cout << "\nAssuming One-step bounded model of total copy number" << endl;
            break;
        }
        case BOUNDA:{
            cout << "\nAssuming One-step bounded model of allele-specific copy number" << endl;
            break;
        }
        case DECOMP:{
            cout << "\nAssuming independent Markov chains" << endl;
            if(!is_total){
                cout << "This model only supports total copy number for now!" << endl;
                exit(1);
            }
            break;
        }
        default: {
            cout << "";
            break;
        }
    }

    int num_total_bins = 0;

    Nchar = 0;
    num_invar_bins = 0;

    map<int, vector<vector<int>>> data;
    cout << "\nReading input copy numbers" << endl;
    if(is_bin){
        data = read_data_var_regions_by_chr(datafile, Ns, cn_max, num_invar_bins, num_total_bins, Nchar, obs_num_wgd, obs_change_chr, sample_max_cn, model, is_total, debug);
        cout << "   Merging consecutive bins in the input" << endl;
    }else{
        if(incl_all){
            cout << "   Using all input segments " << endl;
            correct_bias = 0;
        }else{
            cout << "   Using variable input segments " << endl;
        }
        data = read_data_regions_by_chr(datafile, Ns, cn_max, num_invar_bins, num_total_bins, Nchar, obs_num_wgd, obs_change_chr, sample_max_cn, model, incl_all, is_total);
    }
    // cout << "Number of invariant bins " << num_invar_bins << endl;
    if(num_total_bins == num_invar_bins){
        cout << "There are no variant segments in the input data!" << endl;
        exit(1);
    }
    if(use_repeat){
        cout << "   Using site repeats to speed up likelihood computation " << endl;
    }

    vobs = get_obs_vector_by_chr(data, Ns);

    if(model == MK)   mu = 1 / Nchar;
    vector<double> rates{mu, dup_rate, del_rate, chr_gain_rate, chr_loss_rate, wgd_rate};

    // nodes are in an order suitable for dynamic programming (lower nodes at first, which may be changed after topolgy change)
    int nleaf = Ns + 1;
    for(int k= (nleaf + 1); k < (2 * nleaf - 1); ++k) knodes.push_back(k);
    knodes.push_back(nleaf);

    if(infer_wgd){
        for(int i = 0; i < Ns; ++i){
            cout << "Sample " << i+1 << " probably has " << obs_num_wgd[i] << " WGD events" << endl;
        }
    }

    // Build the table after reading input file
    if(model == DECOMP){
        // adjust_m_max();
        cout << "maximum number of WGD events is " << max_wgd << endl;
        cout << "maximum number of chromosome gain/loss events on one chromosome is " << max_chr_change << endl;
        cout << "maximum number of segment duplication/deletion events is " << max_site_change << endl;
        build_decomp_table(decomp_table, comps, cn_max, m_max, max_wgd, max_chr_change, max_site_change, is_total);
        // build_decomp_table_withm(decomp_table, comps, cn_max, m_max, max_wgd, max_chr_change, max_site_change, is_total);
        cout << "\tNumber of states is " << comps.size() << endl;
        print_decomp_table(decomp_table);
        print_comps(comps);
    }

    max_tobs = *max_element(tobs.begin(), tobs.end());
    lnl_type = {model, cn_max, is_total, cons, max_tobs, age, use_repeat, correct_bias, num_invar_bins, only_seg, infer_wgd, infer_chr, knodes};

    obs_decomp = {m_max, max_wgd, max_chr_change, max_site_change, obs_num_wgd, obs_change_chr};

    int opt_one_branch = 0; // optimize all branches by default
    opt_type = {maxj, tolerance, miter, opt_one_branch};

    string real_tstring = "";   // used for comparison to searched trees
    if(tree_file != ""){
      cout << "reading the real tree" << endl;

      evo_tree real_tree = read_tree_info(tree_file, Ns);
      restore_mutation_rates(real_tree, rates);

      real_tree.print();
      cout << real_tree.make_newick() << endl;

      real_tstring = order_tree_string_uniq(create_tree_string_uniq(real_tree));

      double Ls = 0.0;
      if(model == DECOMP){
          Ls = get_likelihood_decomp(real_tree, vobs, obs_decomp, comps, lnl_type);
      }else{
          // if(debug){
          //     cout << "Parameters for likelihood computation: " << Ns << "\t" << Nchar << "\t" << num_invar_bins << "\t" << model << "\t" << cons << "\t" << cn_max << "\t" << only_seg << "\t" << correct_bias << "\t" << is_total << endl;
          // }
          Ls = get_likelihood_revised(real_tree, vobs, lnl_type);
      }
      cout.precision(dbl::max_digits10);
      cout << "The log likelihood for real tree is: " << Ls << endl;
    }

    if(mode == 0){
      cout << "\nBuilding maximum likelihood tree from copy number profile " << endl;
      if(init_tree == 0){
        cout << "   Using random coalescence trees as initial trees " << endl;
      }else{
        cout << "   Using maximum parsimony trees as initial trees " << endl;
      }

      if(bootstrap){
          cout << "\nDoing bootstapping " << endl;
          get_bootstrap_vector_by_chr(data, vobs, r);
          if(debug){
              cout << " Copy number matrix after bootstapping" << endl;
              for(auto it : vobs){
                  cout << it.first << "\t" << it.second.size() << endl;
              }
          }
      }
      cout << "\nNumber of invariant bins after reading input is: " << num_invar_bins << endl;
      int total_chr = data.rbegin()->first;
      find_ML_tree(real_tstring, total_chr, num_total_bins, ofile, tree_search, Npop, Ngen, init_tree, dir_itrees, max_static, ssize, tolerance, miter, optim, rates, Ne, beta, gtime);

    }else if(mode == 1){
        cout << "Running test on tree " << tree_file << endl;
        int num_invar_bins0 = 0;
        int num_total_bins0 = 0;
        int Nchar0 = 0;
        cout << "Running data without grouping by chromosome" << endl;
        vector<vector<int>> data0 = read_data_var_regions(datafile, Ns, cn_max, num_invar_bins0, num_total_bins0, Nchar0, obs_num_wgd, obs_change_chr, sample_max_cn, model, is_total, debug);
        // Construct the CN matrix
        // cout << "The number of sites used in vobs0: " << data0.size() << endl;
        vector<vector<int>> vobs0;
        for(int nc = 0; nc < data0.size(); ++nc){
          vector<int> obs;
          for(int i = 0; i < Ns; ++i){
            obs.push_back(data0[nc][i+3]);
          }
          vobs0.push_back(obs);
        }

        run_test(tree_file, Ns, num_total_bins, Nchar, model, cn_max, only_seg, correct_bias, is_total, tobs, vobs0, Nchar0, rates, ssize, tolerance, miter);

    }else if(mode == 2){
        cout << "Computing the likelihood of a given tree from copy number profile " << endl;
        double lnl = compute_tree_likelihood(tree_file, decomp_table, Ns, Nchar, num_invar_bins, vobs, tobs, rates, model, cons, cn_max, max_wgd, max_chr_change, max_site_change, only_seg, correct_bias, is_total);
        cout << "The log likelihood of the input tree is " << lnl << endl;
    }else if(mode == 3){
        cout << "Computing maximum likelihood of a given tree from copy number profile " << endl;
        maximize_tree_likelihood(tree_file, ofile, Ns, Nchar, tobs, rates, ssize, tolerance, miter, model, optim, cons, maxj, cn_max, only_seg, correct_bias, is_total);
    }else{
        cout << "Inferring ancestral states of a given tree from copy number profile " << endl;

        evo_tree real_tree = read_tree_info(tree_file, Ns);
        restore_mutation_rates(real_tree, rates);

        MAX_DECOMP max_decomp = {m_max, max_wgd, max_chr_change, max_site_change};

        if(infer_marginal_state){
            cout << "\tInferring marginal ancestral states" << endl;
            string ofile_mrca = ofile + ".mrca.state";
            ofstream fout(ofile_mrca);
            double lnl = 0;
            if(model == DECOMP){
               lnl = reconstruct_marginal_ancestral_state_decomp(real_tree, vobs, comps, obs_decomp, use_repeat, infer_wgd, infer_chr, cn_max, fout, min_asr, is_total);
           }else{
               lnl = reconstruct_marginal_ancestral_state(real_tree, vobs, model, cn_max, use_repeat, is_total, fout, min_asr);
           }
           cout << "The negative log likelihood of the input tree is " << -lnl << endl;
        }

        if(infer_joint_state){
            cout << "\tInferring joint ancestral states" << endl;
            string ofile_state = ofile + ".joint.state";
            ofstream fout_state(ofile_state);
            if(model == DECOMP){
               reconstruct_joint_ancestral_state_decomp(real_tree, vobs, comps, max_decomp, use_repeat, cn_max, fout_state, min_asr, is_total);
           }else{
               reconstruct_joint_ancestral_state(real_tree, vobs, model, cn_max, use_repeat, is_total, m_max, fout_state, min_asr);
           }
        }
    }

    gsl_rng_free(r);
}
