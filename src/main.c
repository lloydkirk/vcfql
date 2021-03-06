#include <argp.h>
#include <float.h>
#include <math.h>
#include <string.h>
#include <stdbool.h>
#include <stdlib.h>
#include <unistd.h>

#include "config.h"
#include "parser.h"

#include "htslib/vcf.h"

static struct argp_option options[] = {
      // FLAGS
      {"verbose",      'v', NULL,     0, "be verbose",                         0},
      {"output",       'o', "OUTPUT", 0, "file to write to (default stdout)",  0},
      {"print-header", 'H', NULL,     0, "print the header and exit",          0},
      {"pass",         'P', NULL,     0, "consider only reads passing filter", 0},
      {0},
};

enum subcommand {NOT_MATCHED, HEADER, FILTER, PARSE};

struct arguments
{
  char *args[1]; // positional args
  char *output;
  char *query_string;
  bool verbose;

  int  subcommand;
};

static int
parse_subcommand(char *raw)
{
  if(strcmp(raw, "header") == 0) {
      return HEADER;
  } else if(strcmp(raw, "filter") == 0){
      return FILTER;
  } else if(strcmp(raw, "parse") == 0){
      return PARSE;
  } else {
      return NOT_MATCHED;
  }
}
    
static error_t
parse_args(int key, char *arg, struct argp_state *state)
{
  struct arguments *args = state->input;
  switch(key)
    {
    /* case argc == 1: argp_state_help(state, stdout, 0); exit(1); */
    case 'v': args->verbose = true; break;
    case 'o': args->output = arg; break;
    case ARGP_KEY_ARG:
      if (state->arg_num >= 2) {
	/* argp_usage(state); */
	argp_error(state, "Too many arguments");
      }

      if (state->arg_num == 0)
	{
	  switch (parse_subcommand(arg))
	    {
	    case HEADER: args->subcommand = HEADER; break; 
	    case FILTER: args->subcommand = FILTER; break; 
	    case PARSE: args->subcommand = PARSE; break; 
	    case NOT_MATCHED:
	      /* TODO: which subcommand? */
	      puts(arg);
	      argp_error(state, "Subcommand not recognized");
	      break;
	    }
	}
      if (state->arg_num == 1)
	{
	  args->args[state->arg_num] = arg;
	}
      break;
    case ARGP_KEY_END:
      if (state->arg_num < 1) {
	/* argp_usage (state); */
	argp_error(state, "A subcommand is required");
      }
      break;
    default:
      return ARGP_ERR_UNKNOWN;
    }
  return 0;
}

const char *argp_program_version = BCF_VERSION;
const char *argp_program_bug_address = BCF_BUG_ADDRESS;
char doc[] = BCF_DOC_STRING;
char args_doc[] = "BCF_FILE";

static struct argp argp = {
   options,
   parse_args,
   args_doc,
   doc,
   0,  // children
   0,  // help_filter
   0   // argp_domain
};

char
*bcf_hdr_type_to_str(int type)
{
  switch(type) {
  case BCF_HL_FLT:  return "HEADER";     break;
  case BCF_HL_INFO: return "INFO";       break;
  case BCF_HL_FMT:  return "FORMAT";     break;
  case BCF_HL_CTG:  return "CONTIG";     break;
  case BCF_HL_STR:  return "STRUCTURED"; break;
  case BCF_HL_GEN:  return "GENERIC";    break;
  }
  return NULL;
}

void
/* print_header_summary(bcf_hdr_t *hdr, struct arguments args) */
print_header_summary(bcf_hdr_t *hdr)
{
  /* fprintf(stderr, "Summary for %s\n", args.args[1]); */
  /* fprintf(stderr, "%i samples\n", bcf_hdr_nsamples(hdr)); */

  int nseq = 0;
  const char **seqnames = NULL;

  /* fprintf(stderr, "Found %i header reqs:\n", hdr->nhrec); */
  seqnames = bcf_hdr_seqnames(hdr, &nseq);
  /* fprintf(stderr, "Found %i sequence names:\n", nseq); */
  if (seqnames != NULL)
    free(seqnames);
  for (int i = 0; i < hdr->nhrec; i++) {

    if (hdr->hrec[i]->nkeys == 0) {
      fprintf(stdout, "%d\t%s\t%s\t%s\t%s\t%s\n",
	      i,
	      hdr->hrec[i]->key,
	      hdr->hrec[i]->value,
	      bcf_hdr_type_to_str(hdr->hrec[i]->type),
	      "NaN",
	      "NaN");
    }
    for (int j = 0; j < hdr->hrec[i]->nkeys; j++) {
      fprintf(stdout, "%d\t%s\t%s\t%s\t%s\t%s\n",
	      i,
	      hdr->hrec[i]->key,
	      hdr->hrec[i]->value ? hdr->hrec[i]->value : "NaN",
	      bcf_hdr_type_to_str(hdr->hrec[i]->type),
	      hdr->hrec[i]->keys[j],
	      hdr->hrec[i]->vals[j]
	      );
    }
  }
}

int
__fcmp(const double x1, const double x2)
{
  int exponent;
  double delta, difference;

  /* Find exponent of largest absolute value */
  {
    double max = (fabs (x1) > fabs (x2)) ? x1 : x2;

    frexp (max, &exponent);
  }

  /* Form a neighborhood of size  2 * delta */
  delta = ldexp (FLT_EPSILON, exponent);

  difference = x1 - x2;

  if (difference > delta)       /* x1 > x2 */
    {
      return 1;
    }
  else if (difference < -delta) /* x1 < x2 */
    {
      return -1;
    }
  else                          /* -delta <= difference <= delta */
    {
      return 0;                 /* x1 ~=~ x2 */
    }
}


void
filter_bcf_and_print(bcf_hdr_t *hdr, htsFile *in_bcf)
{
      bcf1_t *row = bcf_init();

      bcf_hdr_set_samples(hdr, NULL, 0);

      htsFile *out_bcf = bcf_open("-", "w");

      float *val = NULL;
      char *raw_maf_filter = "0.001";
      const double maf_filter = atof(raw_maf_filter);

      int nval = 0;
      while ((bcf_read(in_bcf, hdr, row))>=0) {
	bcf_unpack(row, BCF_UN_STR);
	switch(bcf_get_info_float(hdr, row, "SAS_AF", &val, &nval)) {
	case 0:
	  /* fprintf(stdout, "VALUE:%s\n, is not set", "SAS_AF"); */
	  break;
	case 1:
	  /* fprintf(stdout, "VALUE: %.*e", DECIMAL_DIG, *val); */
	  switch(__fcmp(*val, maf_filter)) {
	  case 0:
	    /* fprintf(stdout, " is equal to %.*e", DECIMAL_DIG, maf_filter); */
	    break;
	  case -1:
	    /* fprintf(stdout, " is less than %.*e", DECIMAL_DIG, maf_filter); */
	    break;
	  case 1:
	    /* fprintf(stdout, " is greater than %.*e", DECIMAL_DIG, maf_filter); */
	    break;
	  }

	  /* fprintf(stdout, " %d\n", float_compare(GREATER, *val, maf_filter)); */

	  /* bcf_write(out_bcf, hdr, row); */
	  break;
	case -1:
	  fprintf(stderr, "no such INFO tag defined in the header\n");
	  break;
	case -2:
	  fprintf(stderr, "clash between types defined in the header"
		  "and encountered in the BCF record\n");
	  break;
	case -3:
	  fprintf(stderr, "tag is not present in the BCF record\n");
	  break;
	}
      }

      free(val);
      bcf_destroy(row);
      bcf_close(out_bcf);
}

void
summarize_arguments(struct arguments args)
{
    printf("Parsed Arguments:\n"
	   "output = %s\n"
	   "bcf_file = %s\n"
	   "subcommand = %d\n"
	   "verbose = %s\n",
	   args.output,
	   args.args[1],
	   args.subcommand,
	   args.verbose ? "yes" : "no");
}

int
main(int argc, char **argv)
{
  /* fprintf(stdout, "EPSILON IS: %.*e\n", DECIMAL_DIG, FLT_EPSILON); */
  struct arguments args;

  args.verbose = false;
  args.output = "-";
  args.args[0] = "";

  argp_parse(&argp, argc, argv, ARGP_IN_ORDER, 0, &args);

  if(args.verbose) {
    summarize_arguments(args);
  }

  if (strlen(args.args[1]) - 1 == 0) {
    puts("No bcf file specified");
    exit(1);
  }

  htsFile   *in_bcf;
  bcf_hdr_t *in_hdr;
  in_bcf = bcf_open(args.args[1], "r");
  in_hdr = bcf_hdr_read(in_bcf);

  switch(args.subcommand)
    {
    case HEADER:
      /* print_header_summary(in_hdr, args); */
      print_header_summary(in_hdr);
      break;

    case FILTER:
      in_bcf = bcf_open(args.args[1], "r");
      filter_bcf_and_print(in_hdr, in_bcf);
      break;

    case PARSE:
      /* if(strlen(args.query_string) == 0) { */
      /*   puts("No query string specified"); */
      /*   exit(1); */
      /* } */

      yyparse(in_bcf, in_hdr);
      break;
    }

  bcf_hdr_destroy(in_hdr);
  bcf_close(in_bcf);
  exit(0);
}
