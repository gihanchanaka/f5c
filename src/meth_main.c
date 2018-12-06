#include "f5c.h"
#include "f5cmisc.h"
#include "logsum.h"
#include <assert.h>
#include <getopt.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

/* Input/processing/output interleave framework : 
unless IO_PROC_NO_INTERLEAVE is set input, processing and output are interleaved
main thread 
1. allocates and loads a databatch
2. create `pthread_processor` thread which will perform the processing 
(note that `pthread_processor` is the process-controller that will spawn user specified number of processing threads)
3. create the `pthread_post_processor` thread that will print the output and free the databatch one the `pthread_processor` is done
4. allocates and load another databatch
5. wait till the previous `pthread_processor` is done and perform 2
6. wait till the previous `pthread_post_processor` is done and perform 3
7. goto 4 untill all the input is processed
*/

//fast logsum data structure
float flogsum_lookup[p7_LOGSUM_TBL]; //todo : get rid of global vars

static struct option long_options[] = {
    {"reads", required_argument, 0, 'r'},   //0 fastq/fasta read file
    {"bam", required_argument, 0, 'b'},     //1 sorted bam file
    {"genome", required_argument, 0, 'g'},  //2 reference genome
    {"threads", required_argument, 0, 't'}, //3 number of threads [8]
    {"batchsize", required_argument, 0,
     'K'}, //4 batchsize - number of reads loaded at once [512]
    {"print", no_argument, 0, 'p'},   //5 prints raw signal (used for debugging)
    {"verbose", no_argument, 0, 'v'}, //6 verbosity level [1]
    {"help", no_argument, 0, 'h'},    //7
    {"version", no_argument, 0, 'V'}, //8
    {"min-mapq", required_argument, 0,
     0}, //9 consider only reads with MAPQ>=min-mapq [30]
    {"secondary", required_argument, 0,
     0}, //10 consider secondary alignments or not [yes]
    {"kmer-model", required_argument, 0,
     0}, //11 custom k-mer model file (used for debugging)
    {"skip-unreadable", required_argument, 0,
     0}, //12 skip any unreadable fast5 or terminate program [yes]
    {"print-events", required_argument, 0,
     0}, //13 prints the event table (used for debugging)
    {"print-banded-aln", required_argument, 0,
     0}, //14 prints the event alignment (used for debugging)
    {"print-scaling", required_argument, 0,
     0}, //15 prints the estimated scalings (used for debugging)
    {"print-raw", required_argument, 0,
     0}, //16 prints the raw signal (used for debugging)
    {"disable-cuda", required_argument, 0,
     0}, //17 disable running on CUDA [no] (only if compiled for CUDA)
    {"cuda-block-size", required_argument, 0, 0}, //18
    {"debug-break", required_argument, 0,
     0}, //19 break after processing the first batch (used for debugging)
    {0, 0, 0, 0}};

static inline int64_t mm_parse_num(const char* str) //taken from minimap2
{
    double x;
    char* p;
    x = strtod(str, &p);
    if (*p == 'G' || *p == 'g')
        x *= 1e9;
    else if (*p == 'M' || *p == 'm')
        x *= 1e6;
    else if (*p == 'K' || *p == 'k')
        x *= 1e3;
    return (int64_t)(x + .499);
}

//parse yes or no arguments
static inline void yes_or_no(opt_t* opt, uint64_t flag, int long_idx,
                             const char* arg,
                             int yes_to_set) //taken from minimap2
{
    if (yes_to_set) {
        if (strcmp(arg, "yes") == 0 || strcmp(arg, "y") == 0) {
            opt->flag |= flag;
        } else if (strcmp(arg, "no") == 0 || strcmp(arg, "n") == 0) {
            opt->flag &= ~flag;
        } else {
            WARNING("option '--%s' only accepts 'yes' or 'no'.",
                    long_options[long_idx].name);
        }
    } else {
        if (strcmp(arg, "yes") == 0 || strcmp(arg, "y") == 0) {
            opt->flag &= ~flag;
        } else if (strcmp(arg, "no") == 0 || strcmp(arg, "n") == 0) {
            opt->flag |= flag;
        } else {
            WARNING("option '--%s' only accepts 'yes' or 'no'.",
                    long_options[long_idx].name);
        }
    }
}

//function that processes a databatch - for pthreads when I/O and processing are interleaved
void* pthread_processor(void* voidargs) {
    pthread_arg2_t* args = (pthread_arg2_t*)voidargs;
    db_t* db = args->db;
    core_t* core = args->core;
    double realtime0 = core->realtime0;

    //process
    process_db(core, db);

    fprintf(stderr, "[%s::%.3f*%.2f] %d Entries processed\n", __func__,
            realtime() - realtime0, cputime() / (realtime() - realtime0),
            db->n_bam_rec);

    //need to inform the output thread that we completed the processing
    pthread_mutex_lock(&args->mutex);
    pthread_cond_signal(&args->cond);
    pthread_mutex_unlock(&args->mutex);

    if (core->opt.verbosity > 1) {
        fprintf(stderr, "[%s::%.3f*%.2f] Signal sent!\n", __func__,
                realtime() - realtime0, cputime() / (realtime() - realtime0));
    }

    pthread_exit(0);
}

//function that prints the output and free - for pthreads when I/O and processing are interleaved
void* pthread_post_processor(void* voidargs) {
    pthread_arg2_t* args = (pthread_arg2_t*)voidargs;
    db_t* db = args->db;
    core_t* core = args->core;
    double realtime0 = core->realtime0;

    //wait until the processing thread has informed us
    pthread_mutex_lock(&args->mutex);
    pthread_cond_wait(&args->cond, &args->mutex);
    pthread_mutex_unlock(&args->mutex);

    if (core->opt.verbosity > 1) {
        fprintf(stderr, "[%s::%.3f*%.2f] Signal got!\n", __func__,
                realtime() - realtime0, cputime() / (realtime() - realtime0));
    }

    //output and free
    output_db(core, db);
    free_db_tmp(db);
    free_db(db);
    free(args);
    pthread_exit(0);
}

int meth_main(int argc, char* argv[]) {
    double realtime0 = realtime();

    //signal(SIGSEGV, sig_handler);

    const char* optstring = "r:b:g:t:K:v:hVp";
    int longindex = 0;
    int32_t c = -1;

    char* bamfilename = NULL;
    char* fastafile = NULL;
    char* fastqfile = NULL;

    FILE* fp_help = stderr;

    opt_t opt;
    init_opt(&opt); //initialise options to defaults

    //parse the user args
    while ((c = getopt_long(argc, argv, optstring, long_options, &longindex)) >=
           0) {
        if (c == 'r') {
            fastqfile = optarg;
        } else if (c == 'b') {
            bamfilename = optarg;
        } else if (c == 'g') {
            fastafile = optarg;
        } else if (c == 'p') {
            opt.flag |= F5C_PRINT_RAW;
        } else if (c == 'K') {
            opt.batch_size = atoi(optarg);
            if (opt.batch_size < 1) {
                ERROR("Batch size should larger than 0. You entered %d",
                      opt.batch_size);
                exit(EXIT_FAILURE);
            }
        } else if (c == 't') {
            opt.num_thread = atoi(optarg);
            if (opt.num_thread < 1) {
                ERROR("Number of threads should larger than 0. You entered %d",
                      opt.num_thread);
                exit(EXIT_FAILURE);
            }
        } else if (c == 'v') {
            opt.verbosity = atoi(optarg);
        } else if (c == 'V') {
            fprintf(stderr, "F5C %s\n", F5C_VERSION);
            exit(EXIT_SUCCESS);
        } else if (c == 'h') {
            fp_help = stdout;
        } else if (c == 0 && longindex == 9) {
            opt.min_mapq =
                atoi(optarg); //todo : check whether this is between 0 and 60
        } else if (c == 0 &&
                   longindex == 10) { //consider secondary mappings or not
            yes_or_no(&opt, F5C_SECONDARY_YES, longindex, optarg, 1);
        } else if (c == 0 && longindex == 11) { //custom model file
            opt.model_file = optarg;
        } else if (c == 0 && longindex == 12) {
            yes_or_no(&opt, F5C_SKIP_UNREADABLE, longindex, optarg, 1);
        } else if (c == 0 && longindex == 13) {
            yes_or_no(&opt, F5C_PRINT_EVENTS, longindex, optarg, 1);
        } else if (c == 0 && longindex == 14) {
            yes_or_no(&opt, F5C_PRINT_BANDED_ALN, longindex, optarg, 1);
        } else if (c == 0 && longindex == 15) {
            yes_or_no(&opt, F5C_PRINT_SCALING, longindex, optarg, 1);
        } else if (c == 0 && longindex == 16) {
            yes_or_no(&opt, F5C_PRINT_RAW, longindex, optarg, 1);
        } else if (c == 0 && longindex == 17) {
#ifdef HAVE_CUDA
            yes_or_no(&opt, F5C_DISABLE_CUDA, longindex, optarg, 1);
#else
            WARNING("%s",
                    "disable-cuda has no effect when compiled for the CPU");
#endif
        } else if (c == 0 && longindex == 18) {
            opt.cuda_block_size =
                atoi(optarg); //todo : warnining for cpu only mode, check limits
        } else if (c == 0 && longindex == 19) {
            yes_or_no(&opt, F5C_DEBUG_BRK, longindex, optarg, 1);
        }
    }

    if (fastqfile == NULL || bamfilename == NULL || fastafile == NULL ||
        fp_help == stdout) {
        fprintf(fp_help, "Usage: f5c call-methylation [OPTIONS] -r reads.fa -b "
                         "alignments.bam -g genome.fa\n");
        fprintf(fp_help, "   -r FILE                 fastq/fasta read file\n");
        fprintf(fp_help, "   -b FILE                 sorted bam file\n");
        fprintf(fp_help, "   -g FILE                 reference genome\n");
        fprintf(fp_help, "   -t INT                  number of threads [%d]\n",
                opt.num_thread);
        fprintf(fp_help,
                "   -K INT                  batch size (number of reads loaded "
                "at once) [%d]\n",
                opt.batch_size);
        fprintf(fp_help, "   -h                      help\n");
        fprintf(fp_help,
                "   --min-mapq INT          minimum mapping quality [%d]\n",
                opt.min_mapq);
        fprintf(fp_help,
                "   --secondary             consider secondary mappings or not "
                "[%s]\n",
                (opt.flag & F5C_SECONDARY_YES) ? "yes" : "no");
        fprintf(fp_help,
                "   --skip-unreadable       skip any unreadable fast5 or "
                "terminate program [%s]\n",
                (opt.flag & F5C_SKIP_UNREADABLE ? "yes" : "no"));
        fprintf(fp_help, "   --verbose INT           verbosity level [%d]\n",
                opt.verbosity);
        fprintf(fp_help, "   --version               print version\n");
#ifdef HAVE_CUDA
        fprintf(fp_help, "   --disable-cuda          disable running on CUDA "
                         "[no] (only if compiled for CUDA)\n");
        fprintf(fp_help, "   --cuda-block-size\n");
#endif

        fprintf(fp_help, "debug options:\n");
        fprintf(fp_help, "   --kmer-model FILE       custom k-mer model file "
                         "(used for debugging)\n");
        fprintf(fp_help, "   --print-events          prints the event table "
                         "(used for debugging)\n");
        fprintf(fp_help, "   --print-banded-aln      prints the event "
                         "alignment (used for debugging)\n");
        fprintf(fp_help, "   --print-scaling         prints the estimated "
                         "scalings (used for debugging)\n");
        fprintf(fp_help, "   --print-raw             prints the raw signal "
                         "(used for debugging)\n");
        fprintf(fp_help, "   --debug-break           break after processing "
                         "the first batch (used for debugging)\n");

        if (fp_help == stdout) {
            exit(EXIT_SUCCESS);
        }
        exit(EXIT_FAILURE);
    }

    //initialise the core data structure
    core_t* core = init_core(bamfilename, fastafile, fastqfile, opt, realtime0);

#ifdef ESL_LOG_SUM
    p7_FLogsumInit();
#endif

#ifdef IO_PROC_NO_INTERLEAVE //If input, processing and output are not interleaved (serial mode)

    //initialise a databatch
    db_t* db = init_db(core);

    int32_t status = db->capacity_bam_rec;
    while (status >= db->capacity_bam_rec) {
        //load a databatch
        status = load_db(core, db);

        fprintf(stderr, "[%s::%.3f*%.2f] %d Entries loaded\n", __func__,
                realtime() - realtime0, cputime() / (realtime() - realtime0),
                status);

        //process a databatch
        process_db(core, db);

        fprintf(stderr, "[%s::%.3f*%.2f] %d Entries processed\n", __func__,
                realtime() - realtime0, cputime() / (realtime() - realtime0),
                status);

        //output print
        output_db(core, db);

        //free temporary
        free_db_tmp(db);

        if (opt.flag & F5C_DEBUG_BRK) {
            break;
        }
    }

    //free the databatch
    free_db(db);

#else //input, processing and output are interleaved (default)

    int32_t status = core->opt.batch_size;
    int8_t first_flag_p = 0;
    int8_t first_flag_pp = 0;
    pthread_t tid_p;  //process thread
    pthread_t tid_pp; //post-process thread

    while (status >= core->opt.batch_size) {
        //init and load a databatch
        db_t* db = init_db(core);
        status = load_db(core, db);

        fprintf(stderr, "[%s::%.3f*%.2f] %d Entries loaded\n", __func__,
                realtime() - realtime0, cputime() / (realtime() - realtime0),
                status);

        if (first_flag_p) { //if not the first time of the "process" wait for the previous "process"
            int ret = pthread_join(tid_p, NULL);
            NEG_CHK(ret);
            if (opt.verbosity > 1) {
                fprintf(stderr, "[%s::%.3f*%.2f] Joined to thread %lu\n",
                        __func__, realtime() - realtime0,
                        cputime() / (realtime() - realtime0), tid_p);
            }
        }
        first_flag_p = 1;

        //set up args
        pthread_arg2_t* pt_arg =
            (pthread_arg2_t*)malloc(sizeof(pthread_arg2_t));
        pt_arg->core = core;
        pt_arg->db = db;
        pt_arg->cond = PTHREAD_COND_INITIALIZER;
        pt_arg->mutex = PTHREAD_MUTEX_INITIALIZER;

        //process thread launch
        int ret =
            pthread_create(&tid_p, NULL, pthread_processor, (void*)(pt_arg));
        NEG_CHK(ret);
        if (opt.verbosity > 1) {
            fprintf(stderr, "[%s::%.3f*%.2f] Spawned thread %lu\n", __func__,
                    realtime() - realtime0,
                    cputime() / (realtime() - realtime0), tid_p);
        }

        if (first_flag_pp) { //if not the first time of the post-process wait for the previous post-process
            int ret = pthread_join(tid_pp, NULL);
            NEG_CHK(ret);
            if (opt.verbosity > 1) {
                fprintf(stderr, "[%s::%.3f*%.2f] Joined to thread %lu\n",
                        __func__, realtime() - realtime0,
                        cputime() / (realtime() - realtime0), tid_pp);
            }
        }
        first_flag_pp = 1;

        //post-process thread launch (output and freeing thread)
        ret = pthread_create(&tid_pp, NULL, pthread_post_processor,
                             (void*)(pt_arg));
        NEG_CHK(ret);
        if (opt.verbosity > 1) {
            fprintf(stderr, "[%s::%.3f*%.2f] Spawned thread %lu\n", __func__,
                    realtime() - realtime0,
                    cputime() / (realtime() - realtime0), tid_pp);
        }

        if (opt.flag & F5C_DEBUG_BRK) {
            break;
        }
    }

    //final round
    int ret = pthread_join(tid_p, NULL);
    NEG_CHK(ret);
    if (opt.verbosity > 1) {
        fprintf(stderr, "[%s::%.3f*%.2f] Joined to thread %lu\n", __func__,
                realtime() - realtime0, cputime() / (realtime() - realtime0),
                tid_p);
    }
    ret = pthread_join(tid_pp, NULL);
    NEG_CHK(ret);
    if (opt.verbosity > 1) {
        fprintf(stderr, "[%s::%.3f*%.2f] Joined to thread %lu\n", __func__,
                realtime() - realtime0, cputime() / (realtime() - realtime0),
                tid_pp);
    }

#endif

    // fprintf(stderr, "[post-run summary] total reads: %d, unparseable: %d, qc fail: %d, could not calibrate: %d, no alignment: %d, bad fast5: %d\n",
    //         g_total_reads, g_unparseable_reads, g_qc_fail_reads, g_failed_calibration_reads, g_failed_alignment_reads, g_bad_fast5_file);

#ifdef SECTIONAL_BENCHMARK
    fprintf(stderr, "\n[%s] Events time: %.3f sec", __func__, core->event_time);
    fprintf(stderr, "\n[%s] Alignment time: %.3f sec", __func__,
            core->align_time);
#    ifdef HAVE_CUDA
    if (!(core->opt.flag & F5C_DISABLE_CUDA)) {
        fprintf(stderr, "\n[%s] Alignment kernel only time: %.3f sec", __func__,
                core->align_kernel_time);
        fprintf(stderr, "\n[%s] Alignment pre kernel only time: %.3f sec",
                __func__, core->align_pre_kernel_time);
        fprintf(stderr, "\n[%s] Alignment core kernel only time: %.3f sec",
                __func__, core->align_core_kernel_time);
        fprintf(stderr, "\n[%s] Alignment post kernel only time: %.3f sec",
                __func__, core->align_post_kernel_time);
        fprintf(stderr, "\n[%s] Alignment preprocess time: %.3f sec", __func__,
                core->align_cuda_preprocess);
        fprintf(stderr, "\n[%s] Alignment malloc time: %.3f sec", __func__,
                core->align_cuda_malloc);
        fprintf(stderr, "\n[%s] Alignment data move time: %.3f sec", __func__,
                core->align_cuda_memcpy);
        fprintf(stderr, "\n[%s] Alignment post process time: %.3f sec",
                __func__, core->align_cuda_postprocess);
        fprintf(
            stderr,
            "\n[%s] Alignment (ultra-long) extra CPU process time: %.3f sec",
            __func__, core->extra_load_cpu);
    }
#    endif
    fprintf(stderr, "\n[%s] Estimate scaling time: %.3f sec", __func__,
            core->est_scale_time);
    fprintf(stderr, "\n[%s] Call methylation time: %.3f sec", __func__,
            core->meth_time);

#endif

    //free the core data structure
    free_core(core);

    return 0;
}
