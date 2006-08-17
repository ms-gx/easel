/* esl_tree.c
 * Phylogenetic trees.
 * 
 * SVN $Id$
 * SRE, Tue May  2 13:54:30 2006 [St. Louis]
 */
#ifndef ESL_TREE_INCLUDED
#define ESL_TREE_INCLUDED


/* Object: ESL_TREE
 *
 * All trees are represented as rooted trees, starting from
 * node 0. For N taxa, there are N-1 internal nodes, numbered
 * 0..N-2. Taxa on leaves are numbered 0..N-1, and represented
 * in <parent>, <left>, <right> as negative numbers.
 * 
 */
typedef struct {
  int   N;		/* number of taxa */

  /* (Mandatory) information in the internal nodes of a rooted tree.
   * There are N-1 nodes, numbered 0..N-2, with the root at 0.
   */
  int    *parent;	/* index of parent node: 0..N-2=nodes  */
  int    *left;		/* index of left child:  -(N-1)..0=taxa; 1..N-2=nodes */
  int    *right;	/* index of right child: -(N-1)..0=taxa; 1..N-2=nodes */
  double *ld;	        /* left branch length  */
  double *rd;	        /* right branch length */
} ESL_TREE;



/* 1. The ESL_TREE object.
 */
extern ESL_TREE *esl_tree_Create(int ntaxa);
extern void      esl_tree_Destroy(ESL_TREE *T);

/* 2. Clustering algorithms for distance-based tree construction.
 */
extern int esl_tree_UPGMA(ESL_DMATRIX *D, ESL_TREE **ret_T);
extern int esl_tree_WPGMA(ESL_DMATRIX *D, ESL_TREE **ret_T);
extern int esl_tree_SingleLinkage(ESL_DMATRIX *D, ESL_TREE **ret_T);
extern int esl_tree_CompleteLinkage(ESL_DMATRIX *D, ESL_TREE **ret_T);

#endif /*!ESL_TREE_INCLUDED*/

/*****************************************************************
 * @LICENSE@
 *****************************************************************/