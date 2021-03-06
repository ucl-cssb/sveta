# library(tidyverse)
suppressMessages(library(ggtree))
suppressMessages(library(ape))
suppressMessages(library(tools))
suppressMessages(library(ggplot2))
suppressMessages(library(optparse))


# This script is used to plot phylogenetic trees:
# 1) plotting a single tree file with tips as 1 to n or with provided labels
# 2) plotting a single tree file with bootstrapping support
# 3) plotting all tree files in a directory

# d is a data frame with 3 columns: start, end, branch length
# Leaves must be encoded from 1 to nleaf
make.tree <- function(d, labels = NA, digit = 2) {
  nedge <- nrow(d)
  nleaf <- (nedge + 2)/2
  nnode <- nleaf - 1

  mytree <- list()
  mytree$edge <- as.matrix(d[, c(1, 2)])
  mytree$Nnode <- as.integer(nnode)

  if(is.na(labels)){
     mytree$tip.label <- paste(1:nleaf)
  }
  else{
    mytree$tip.label <- labels
  }

  # control precision of branch length
  mytree$edge.length <- round(d[, 3], digit)
  class(mytree) <- "phylo"
  checkValidPhylo(mytree)
  return(mytree)
}


# Add mutation rate for each plot
get.plot.title <- function(mut_rate, dup_rate, del_rate){
  title <- ""
  if(mut_rate>0){
    title <- paste0("mutation rate ", mut_rate)
  }
  if(dup_rate>0){
    title <- paste0("duplication rate ", dup_rate,"\ndeletion rate ", del_rate)
  }
  return(title)
}


plot.tree <- function(tree, title = "") {
  p <- ggtree(tree)  #+ geom_rootedge()
  p <- p + geom_tiplab()
  p <- p + geom_text2(aes(subset = !isTip, label = node), hjust = -0.3)
  edge = data.frame(tree$edge, edge_num = 1:nrow(tree$edge), edge_len = tree$edge.length)
  colnames(edge) = c("parent", "node", "edge_num", "edge_len")
  p <- p %<+% edge + geom_text(aes(x = branch, label = edge_len), nudge_y = 0.1) + ggtitle(title)
  print(p)
}

# Plot tree with xlim specified to show full tip labels
plot.tree.xlim <- function(tree, title = "") {
  p <- ggtree(tree, size = 0.5, linetype = 1)  #+ geom_rootedge()
  # Add margin to show full name of labels  if (is.na(tree.max))
  tree.max = max(node.depth.edgelength(tree)) + 20
  p <- p + geom_tiplab(align = TRUE) + theme_tree2() + xlim(NA, tree.max)
  # p <- p + geom_text2(aes(subset=!isTip,label = node), hjust=-.3)
  edge = data.frame(tree$edge, edge_num = 1:nrow(tree$edge), edge_len = tree$edge.length)
  colnames(edge) = c("parent", "node", "edge_num", "edge_len")
  p <- p %<+% edge + geom_text(aes(x = branch, label = edge_len), nudge_y = 0.1) + ggtitle(title)
  print(p)
}

# Plot tree with xlim specified with age forwards
plot.tree.xlim.age <- function(tree, diff, age, title = "") {
  p <- ggtree(tree, size = 0.5, linetype = 1)  #+ geom_rootedge()
  # Add margin to show full name of labels
  tree.max = age + 10
  # Shift all nodes by the difference between age util the first sample and node time of the first sample
  p$data$x = p$data$x + diff
  p <- p + geom_tiplab(align = TRUE) + theme_tree2() + xlim(0, tree.max)
  # p <- p + geom_text2(aes(subset=!isTip,label = node), hjust=-.3)
  edge = data.frame(tree$edge, edge_num = 1:nrow(tree$edge), edge_len = tree$edge.length)
  colnames(edge) = c("parent", "node", "edge_num", "edge_len")
  p <- p %<+% edge + geom_text(aes(x = branch, label = edge_len), nudge_y = 0.1, nudge_x = diff) + ggtitle(title)
  print(p)
}


plot.tree.bootstrap <- function(tree, fout, title = ""){
  pdf(fout)

  p <- ggtree(tree) #+ geom_rootedge()
  # support <- character(length(tree$node.label))
  # #The following three lines define your labeling scheme.
  # support[tree$node.label >= 95] <- "red"
  # support[tree$node.label < 95 & tree$node.label >= 70] <- "pink"
  # support[tree$node.label < 70] <- "blue"
  tree.max= max(node.depth.edgelength(tree)) + 20
  p <- p + geom_tiplab(align = TRUE) + theme_tree2() + xlim(NA, tree.max)
  p <- p + geom_text2(aes(subset=!isTip, label=label, hjust=-.3, color="red"))
  edge = data.frame(tree$edge, edge_num = 1:nrow(tree$edge), edge_len = tree$edge.length)
  colnames(edge)=c("parent", "node", "edge_num", "edge_len")
  p <- p %<+% edge + geom_text(aes(x = branch, label = edge_len), nudge_y = 0.1) + ggtitle(title)
  print(p)

  dev.off()
}


# Plot bootstrapped tree with x-axis being patient age
plot.tree.bootstrap.age <- function(tree, fout, diff, age, title = ""){
  pdf(fout)

  p <- ggtree(tree) #+ geom_rootedge()
  # support <- character(length(tree$node.label))
  # #The following three lines define your labeling scheme.
  # support[tree$node.label >= 95] <- "red"
  # support[tree$node.label < 95 & tree$node.label >= 70] <- "pink"
  # support[tree$node.label < 70] <- "blue"
  #tree.max= max(node.depth.edgelength(tree)) + 20
  # Add margin to show full name of labels
  tree.max = age + 10
  # Shift all nodes by the difference between age util the first sample and node time of the first sample
  p$data$x = p$data$x + diff
  p <- p + geom_tiplab(align = TRUE) + theme_tree2() + xlim(0, tree.max)
  p <- p + geom_text2(aes(subset=!isTip, label=label, hjust=-.3, color="red"))
  edge = data.frame(tree$edge, edge_num = 1:nrow(tree$edge), edge_len = tree$edge.length)
  colnames(edge)=c("parent", "node", "edge_num", "edge_len")
  p <- p %<+% edge + geom_text(aes(x = branch, label = edge_len), nudge_y = 0.1, nudge_x = diff) + ggtitle(title)
  print(p)

  dev.off()
}


# Prepare the tree for plotting
get.tree <- function(tree_file, out_file = "", branch_num = 0, labels = NA){
  dd <- read.table(tree_file, header = T)

  dir <- dirname(tree_file)
  stub <- file_path_sans_ext(basename(tree_file))
  if(out_file!=""){
    fout <- out_file
  }
  else{
    if(branch_num == 1) {
      mfix = paste0(stub, "-mnum")
    }
    else{
      mfix = stub
    }
    fout <- file.path(dir, paste("plot-",mfix,".pdf",sep=""))
  }
  cat("\n\nrunning over", stub, fout, "\n", sep = "\t")

  # dd$start <- dd$start + 1 dd$end <- dd$end + 1
  if (branch_num == 0) {
    dd$length <- as.numeric(dd$length)
    small_col <- which(dd$length < 1e-3)
    if(length(small_col)>0){
      dd[small_col, ]$length <- 0
    }
    dd <- dd[, c(1, 2, 3)]
  }
  if (branch_num == 1) {
    dd$nmut <- as.numeric(dd$nmut)
    dd <- dd[, c("start", "end", "nmut")]
  }
  mytree <- make.tree(dd, labels)

  return(list(mytree = mytree, fout = fout))
}


print.tree <- function(mytree, fout, tree_style, time_file="", title = "") {
  pdf(fout)

  if(tree_style=="simple"){
    plot.tree(mytree, title)
  }else if(tree_style=="xlim"){
    plot.tree.xlim(mytree, title)
  }else if(tree_style=="age"){
    # The lengths of tips are at the beginning
    elens=node.depth.edgelength(mytree)
    stime = read.table(time_file, header = F)
    names(stime) = c("sample","tdiff","age")
    s1_info = stime[stime$tdiff==0,]
    diff = s1_info$age- elens[s1_info$sample]
    age = max(stime$age)
    plot.tree.xlim.age(mytree, diff, age, title)
  }

  dev.off()
  # ggsave(file.out, width = 11.69, height = 8.27, units="in", limitsize = FALSE)
}


get.labels <- function(annot_file){
  labels = NA
  if(annot_file!=""){
    da <- read.table(annot_file,header = T,stringsAsFactors = F)
    labels = as.character(da$sample)
  }
  return(labels)
}


option_list = list(
  make_option(c("-f", "--tree_file"), type="character", default="",
              help="dataset file name [default=%default]", metavar="character"),
  make_option(c("-o", "--out_file"), type="character", default="",
              help="The name of output file [default=%default]", metavar="character"),
  make_option(c("-a", "--annot_file"), type="character", default="",
              help="The file containing the labels of tip nodes [default=%default]", metavar="character"),
  make_option(c("", "--time_file"), type="character", default="",
              help="The file containing the sampling time information [default=%default]", metavar="character"),
  make_option(c("-d", "--tree_dir"), type="character", default="",
              help="The directory containing all the tree files to plot [default=%default]", metavar="character"),
  make_option(c("-s", "--bstrap_dir"), type="character", default="",
              help="The directory containing all the bootstrapping tree files [default=%default]", metavar="character"),
  make_option(c("-p", "--pattern"), type="character", default="",
              help="The naming pattern of tree files [default=%default]", metavar="character"),
	make_option(c("-b", "--branch_num"), type="integer", default = 0,
              help="The type of values on branches (0: time in year, 1: mutation number) [default=%default]", metavar="integer"),
  make_option(c("-m", "--mut_rate"), type="numeric", default = 0,
              help="The mutation rate of somatic chromosomal aberrations [default=%default]", metavar="numeric"),
  make_option(c("-u", "--dup_rate"), type="numeric", default = 0,
              help="The segment duplication rate of somatic chromosomal aberrations [default=%default]", metavar="numeric"),
  make_option(c("-e", "--del_rate"), type="numeric", default = 0,
              help="The segment deletion rate of somatic chromosomal aberrations [default=%default]", metavar="numeric"),
  make_option(c("-w", "--with_title"), type="integer", default = 0,
              help="Showing title or not (0: without title, 1: with title) [default=%default]", metavar="integer"),
  # make_option(c("-g", "--use_age"), type="integer", default = 0,
  #             help="Showing x-axis as the real age (0: default (root as beginning time), 1: x-axis as real age of the patient) [default=%default]", metavar="integer"),
  make_option(c("-t", "--plot_type"), type="character", default="single",
              help="The type of plot, including: all (plotting all tree files in a directory), single (plotting a single tree file), and bootstrap (plotting a single tree file with bootstrapping support) [default=%default]", metavar="character"),
  make_option(c("-l", "--tree_style"), type="character", default="simple",
              help="The style of tree plot, including: simple (a simple tree with tip labels and branch lengths), xlim (adding xlim to the tree), age (x-axis as real age of the patient) [default=%default]", metavar="character")
);

opt_parser = OptionParser(option_list = option_list);
opt = parse_args(opt_parser);

tree_file = opt$tree_file
out_file = opt$out_file
plot_type = opt$plot_type
tree_dir = opt$tree_dir
pattern = opt$pattern
bstrap_dir = opt$bstrap_dir
tree_style = opt$tree_style
annot_file = opt$annot_file
branch_num = opt$branch_num
mut_rate = opt$mut_rate
dup_rate = opt$dup_rate
del_rate = opt$del_rate
with_title =  opt$with_title
time_file = opt$time_file
# cat("Parameters used here:\n")
# cat("tree_file:", tree_file, "\n")
# cat("plot_type:", plot_type, "\n")
# cat("tree_dir:", tree_dir, "\n")
# cat("branch_num:", branch_num, "\n")
# cat("pattern:", pattern, "\n")

title = ""
if(with_title==1){
  title = get.plot.title(mut_rate, dup_rate, del_rate);
}

if (plot_type == "all"){
  # dir <- '../sim-data/'
  dir <- tree_dir
  cat(paste0("Plotting all trees in directory ", dir, "\n"))
  if(pattern == ""){
    files <- list.files(dir, "^sim\\-data\\-\\d+\\-tree.txt")
  }
  else{
    files <- list.files(dir, pattern = glob2rx(pattern))
  }
  #print(files)

  for (f in files) {
    cat("running on:", f, "\n")
    fname = file.path(dir, f)
    tree = get.tree(fname, out_file = out_file, branch_num = branch_num)
    print.tree(tree$mytree, tree$fout, tree_style, time_file = time_file, title = title)
  }

}else if (plot_type == "single"){
  cat(paste0("Plotting the tree in ", tree_file))
  labels = get.labels(annot_file)
  tree = get.tree(tree_file, out_file = out_file, branch_num = branch_num, labels = labels)
  print.tree(tree$mytree, tree$fout, tree_style, time_file = time_file, title = title)

} else if (plot_type == "bootstrap"){
  cat(paste0("Plotting bootstrap values for the tree in ", tree_file))

  labels = get.labels(annot_file)
  tree = get.tree(tree_file, out_file = out_file, branch_num = branch_num, labels = labels)
  mytree = tree$mytree
  fout = tree$fout

  btrees = list()
  cat("Patterns to match bootstrapping trees: ", pattern, "\n")
  files = list.files(path = bstrap_dir, pattern = glob2rx(pattern), recursive = F)
  for (i in 1:length(files)){
    fname = file.path(bstrap_dir, files[i])
    dt = read.table(fname,header = T)
    btree = make.tree(dt, labels)
    btrees[[i]] = btree
  }

  # prop.clades calls internally prop.part with the option check.labels = TRUE
  clad <- prop.clades(mytree, btrees, rooted = TRUE)
  clad[is.na(clad)] = 0
  mytree$node.label = as.integer(clad * 100 / length(files))

  if(tree_style=="age"){
    # The lengths of tips are at the beginning
    elens=node.depth.edgelength(mytree)
    stime = read.table(time_file, header = F)
    names(stime) = c("sample","tdiff","age")
    s1_info = stime[stime$tdiff==0,]
    diff = s1_info$age- elens[s1_info$sample]
    age = max(stime$age)
    plot.tree.bootstrap.age(mytree, fout, diff, age, title)
  }else{
    plot.tree.bootstrap(mytree, fout, title)
  }

} else{
  message("plotting type not supported!")
}
