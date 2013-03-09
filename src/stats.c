#include <cctk.h>
#include <cctk_Arguments.h>
#include <cctk_Parameters.h>

#include <assert.h>
#include <math.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <papi.h>


// For backward compatibility
#if PAPI_VERSION_MAJOR(PAPI_VERSION) <= 4

// Taken (and modified) from PAPI 5.1.0.1, file src/papi.c
static int PAPI_add_named_event(int EventSet, char *EventName)
{
  int ret;
  int code;
  ret = PAPI_event_name_to_code(EventName, &code);
  if (ret != PAPI_OK) return ret;
  ret = PAPI_add_event(EventSet, code);
  return ret;
}
#endif

////////////////////////////////////////////////////////////////////////////////



// User-defined event sets
#define num_eventsets 5
static const char *restrict const eventset_names[num_eventsets] = {
  "flops", "ipc", "icache", "dcache", "memory",
};
static int eventsets[num_eventsets];

// The event set we are using
static int eventset_index = -1;
// The PAPI component of this event set
static int component = -1;
// Number of events in this event set
static int num_events = -1;
// Events in this event set
static int *restrict events = NULL;
// Event index of PAPI_FP_OPS and PAPI_DP_OPS
static int fp_ops_index = -1;
static int dp_ops_index = -1;



////////////////////////////////////////////////////////////////////////////////



// Remember timer values when PAPI counters were last reset
static long long last_cyc = -1, last_nsec = -1;



#if 0
// This is recommended for Linux, but e.g. doesn't work on OSX
#include <pthread.h>
static unsigned long thread_id(void)
{
  return pthread_self();
}
#endif

// This works only if no threads are created/destroyed
// (if the number of threads doesn't change)
#include <omp.h>
static unsigned long thread_id(void)
{
  return omp_get_thread_num();
}



// Output a a debug message
static void outinfo(const char *const function)
{
  DECLARE_CCTK_PARAMETERS;
  if (verbose) {
    printf("[%s]\n", function);
  }
}

// Check for an error, and output an error message if there was one
static void chkerr(const int ierr, const char *const function, ...)
  CCTK_ATTRIBUTE_FORMAT(printf, 2, 3);
static void chkerr(const int ierr, const char *const function, ...)
{
  if (ierr<0) {
    va_list ap;
    va_start(ap, function);
    printf("ERROR %d [%s] in ", ierr, PAPI_strerror(ierr));
    vprintf(function, ap);
    printf("\n");
    va_end(ap);
  }
}



void PAPI_init(CCTK_ARGUMENTS)
{
  DECLARE_CCTK_ARGUMENTS;
  DECLARE_CCTK_PARAMETERS;
  
  int ierr;
  
  CCTK_INFO("Initialising PAPI");
  
  // Initialise PAPI
  
  outinfo("PAPI_library_init");
  ierr = PAPI_library_init(PAPI_VER_CURRENT);
  chkerr(ierr, "PAPI_library_init");
  if (ierr != PAPI_VER_CURRENT) {
    CCTK_ERROR("PAPI library version mismatch");
  }
  
  if (per_thread_statistics) {
    outinfo("PAPI_thread_init");
    ierr = PAPI_thread_init(thread_id);
    chkerr(ierr, "PAPI_thread_init");
  }
  
  if (use_multiplexing) {
    outinfo("PAPI_multiplex_init");
    ierr = PAPI_multiplex_init();
    chkerr(ierr, "PAPI_multiplex_init");
  }
  
  // Get some basic information
  
  outinfo("PAPI_num_components");
  int num_components = ierr = PAPI_num_components();
  chkerr(ierr, "PAPI_num_components");
  if (ierr<0) num_components = 0;
  if (verbose) {
    printf("There are %d PAPI components\n", num_components);
  }
  
  if (verbose) {
    for (int n=0; n<num_components; ++n) {
      outinfo("PAPI_get_component_info");
      const PAPI_component_info_t *const info = PAPI_get_component_info(n);
      printf("PAPI component #%d:\n", n);
      printf("   name: %s\n", info->name);
#if PAPI_VERSION_MAJOR(PAPI_VERSION) >= 5
      printf("   short_name: %s\n", info->short_name);
      printf("   description: %s\n", info->description);
#endif
      printf("   num_cntrs: %d\n", info->num_cntrs);
      printf("   num_mpx_cntrs: %d\n", info->num_mpx_cntrs);
      printf("   num_preset_events: %d\n", info->num_preset_events);
      printf("   num_native_events: %d\n", info->num_native_events);
    }
  }
  
  component = 0;
  if (verbose) {
    printf("Using PAPI component %d\n", component);
  }
  
  outinfo("PAPI_num_counters");
  int num_counters = ierr = PAPI_num_counters();
  chkerr(ierr, "PAPI_num_counters");
  if (ierr<0) num_counters = 0;
  if (verbose) {
    printf("There are %d PAPI counters\n", num_counters);
  }
  
  // Translate user event names to event sets
  
  const char *const eventset_eventnames[num_eventsets] = {
    events_flops, events_ipc, events_icache, events_dcache, events_memory,
  };
  
  for (int n=0; n<num_eventsets; ++n) {
    if (verbose) {
      printf("Creating event set %s:\n", eventset_names[n]);
    }
    outinfo("PAPI_create_eventset");
    eventsets[n] = PAPI_NULL;
    ierr = PAPI_create_eventset(&eventsets[n]);
    chkerr(ierr, "PAPI_create_eventset");
    outinfo("PAPI_assign_eventset_component");
    ierr = PAPI_assign_eventset_component(eventsets[n], component);
    chkerr(ierr, "PAPI_assign_eventset_component");
    if (use_multiplexing) {
      outinfo("PAPI_set_multiplex");
      ierr = PAPI_set_multiplex(eventsets[n]);
      chkerr(ierr, "PAPI_set_multiplex");
    }
    
    char *const eventnames = strdup(eventset_eventnames[n]);
    const char *const sep = ", ";
    char *lasts;
    for (char *event_name = strtok_r(eventnames, sep, &lasts);
         event_name;
         event_name = strtok_r(NULL, sep, &lasts))
    {
      if (verbose) {
        printf("   event %s\n", event_name);
      }
      outinfo("PAPI_add_named_event");
      ierr = PAPI_add_named_event(eventsets[n], event_name);
      chkerr(ierr, "PAPI_add_named_event[%s]", event_name);
    }
    free(eventnames);
  }
  
  eventset_index = 0;
  if (verbose) {
    printf("Using PAPI event set %s\n", eventset_names[eventset_index]);
  }
  
  outinfo("PAPI_num_events");
  num_events = ierr = PAPI_num_events(eventsets[eventset_index]);
  chkerr(ierr, "PAPI_num_events");
  if (ierr<0) num_events = 0;
  
  outinfo("PAPI_list_events");
  events = malloc(num_events * sizeof *events);
  ierr = PAPI_list_events(eventsets[eventset_index], events, &num_events);
  chkerr(ierr, "PAPI_list_events");
  
  for (int i=0; i<num_events; ++i) {
    if (events[i] == PAPI_FP_OPS) fp_ops_index = i;
    if (events[i] == PAPI_DP_OPS) dp_ops_index = i;
  }
  
  outinfo("PAPI_start");
  ierr = PAPI_start(eventsets[eventset_index]);
  chkerr(ierr, "PAPI_start");
  
  outinfo("PAPI_get_real_cyc");
  last_cyc = PAPI_get_real_cyc();
  outinfo("PAPI_get_real_nsec");
  last_nsec = PAPI_get_real_nsec();
}



static void output_stats(CCTK_ARGUMENTS)
{
  DECLARE_CCTK_ARGUMENTS;
  DECLARE_CCTK_PARAMETERS;
  
  int ierr;
  
  CCTK_INFO("PAPI Statistics:");
  
  outinfo("PAPI_get_real_nsec");
  const long long nsec = PAPI_get_real_nsec();
  
  outinfo("PAPI_read_ts");
  long long cyc;
  long long values[num_events];
  ierr = PAPI_read_ts(eventsets[eventset_index], values, &cyc);
  chkerr(ierr, "PAPI_read_ts");
  if (ierr<0) cyc = PAPI_get_real_cyc();
  
  const long long elapsed_nsec = nsec - last_nsec;
  const long long elapsed_cyc = cyc - last_cyc;
  last_nsec = nsec;
  last_cyc = cyc;
  
  printf("   time              %20.9f sec\n", elapsed_nsec / 1.0e+9);
  printf("   cycles            %20.9f Gcyc\n", elapsed_cyc / 1.0e+9);
  for (int i=0; i<num_events; ++i) {
    outinfo("PAPI_event_code_to_name");
    char eventname[PAPI_MAX_STR_LEN];
    ierr = PAPI_event_code_to_name(events[i], eventname);
    chkerr(ierr, "PAPI_event_code_to_name");
    printf("   %-15s   %20.9f cyc^(-1)\n",
           eventname, 1.0 * values[i] / elapsed_cyc);
  }
}

void PAPI_output_stats_analysis(CCTK_ARGUMENTS)
{
  DECLARE_CCTK_ARGUMENTS;
  DECLARE_CCTK_PARAMETERS;
  
  if (out_every == -1) return;
  if (cctk_iteration > 0) {
    if (out_every == 0) return;
    if (cctk_iteration % out_every > 0) return;
  }
  
  output_stats(CCTK_PASS_CTOC);
}

void PAPI_output_stats_terminate(CCTK_ARGUMENTS)
{
  DECLARE_CCTK_ARGUMENTS;
  DECLARE_CCTK_PARAMETERS;
  
  if (out_every == -1) return;
  if (out_every > 0) return;
  
  output_stats(CCTK_PASS_CTOC);
}



////////////////////////////////////////////////////////////////////////////////



static const char *const clock_name = "PAPI";

typedef long long papi_counter_t;
typedef struct {
  papi_counter_t nsec, cyc, fp_ops, dp_ops;
} papi_counters_t;
typedef struct {
  papi_counters_t accum, snapshot;
  bool running;
} papi_clock_t;
static const int num_clock_vals =
  sizeof(papi_counters_t) / sizeof(papi_counter_t);

static void clock_get_papi_counters(papi_counters_t *restrict const counters)
{
  int ierr;
  const long long nsec = PAPI_get_real_nsec();
  long long cyc;
  long long values[num_events];
  ierr = PAPI_read_ts(eventsets[eventset_index], values, &cyc);
  // chkerr(ierr, "PAPI_read_ts");
  if (ierr<0) cyc = PAPI_get_real_cyc();
  const long long fp_ops = fp_ops_index>=0 ? values[fp_ops_index] : 0;
  const long long dp_ops = dp_ops_index>=0 ? values[dp_ops_index] : 0;
  counters->nsec   = nsec;
  counters->cyc    = cyc;
  counters->fp_ops = fp_ops;
  counters->dp_ops = dp_ops;
}



static void *clock_create(const int timernum)
{
  papi_clock_t *restrict const data = malloc(sizeof(papi_clock_t));
  assert(data);
  data->accum.nsec   = 0;
  data->accum.cyc    = 0;
  data->accum.fp_ops = 0;
  data->accum.dp_ops = 0;
  data->running = false;
  return data;
}

static void clock_destroy(const int timernum, void *restrict const data)
{
  free(data);
}

static void clock_start(const int timernum, void *restrict const data_)
{
  papi_clock_t *restrict const data = data_;
  assert(!data->running);
  clock_get_papi_counters(&data->snapshot);
  data->running = true;
}

static void clock_stop(const int timernum, void *restrict const data_)
{
  papi_clock_t *restrict const data = data_;
  // Apparently clocks can be created in a "running" state?
  if (!data->running) return;
  assert(data->running);
  papi_counters_t current;
  clock_get_papi_counters(&current);
  data->running = false;
  data->accum.nsec   += current.nsec   - data->snapshot.nsec;
  data->accum.cyc    += current.cyc    - data->snapshot.cyc;
  data->accum.fp_ops += current.fp_ops - data->snapshot.fp_ops;
  data->accum.dp_ops += current.dp_ops - data->snapshot.dp_ops;
}

static void clock_reset(const int timernum, void *restrict const data_)
{
  papi_clock_t *restrict const data = data_;
  data->accum.nsec   = 0;
  data->accum.cyc    = 0;
  data->accum.fp_ops = 0;
  data->accum.dp_ops = 0;
}

static void clock_get(const int timernum,
                      void *restrict const data_,
                      cTimerVal *restrict const vals)
{
  papi_clock_t *restrict const data = data_;
  
  vals[0].type       = val_double;
  vals[0].heading    = "PAPI_time";
  vals[0].units      = "sec";
  vals[0].val.d      = data->accum.nsec / 1.0e+9;
  vals[0].seconds    = data->accum.nsec / 1.0e+9;
  vals[0].resolution = 1.0e-9;
  
  vals[1].type       = val_double;
  vals[1].heading    = "PAPI_cycles";
  vals[1].units      = "Gcyc";
  vals[1].val.d      = data->accum.cyc / 1.0e+9;
  vals[1].seconds    = data->accum.cyc / 1.0e+9;
  vals[1].resolution = 1.0e-9;
  
  vals[2].type       = val_double;
  vals[2].heading    = "PAPI_flop";
  vals[2].units      = "Gflop";
  vals[2].val.d      = data->accum.fp_ops / 1.0e+9;
  vals[2].seconds    = data->accum.fp_ops / 1.0e+9;
  vals[2].resolution = 1.0e-9;
  
  vals[3].type       = val_double;
  vals[3].heading    = "PAPI_dp_flop";
  vals[3].units      = "Gflop";
  vals[3].val.d      = data->accum.dp_ops / 1.0e+9;
  vals[3].seconds    = data->accum.dp_ops / 1.0e+9;
  vals[3].resolution = 1.0e-9;
}

static void clock_set(const int timernum,
                      void *restrict const data_,
                      cTimerVal *restrict const vals)
{
  papi_clock_t *restrict const data = data_;
  
  assert(vals[0].type == val_double);
  data->accum.nsec = llrint(vals[0].val.d * 1.0e+9);
  
  assert(vals[1].type == val_double);
  data->accum.cyc = llrint(vals[1].val.d * 1.0e+9);
  
  assert(vals[2].type == val_double);
  data->accum.fp_ops = llrint(vals[2].val.d * 1.0e+9);
  
  assert(vals[3].type == val_double);
  data->accum.dp_ops = llrint(vals[3].val.d * 1.0e+9);
}



void PAPI_register_clock(CCTK_ARGUMENTS)
{
  DECLARE_CCTK_ARGUMENTS;
  
  CCTK_INFO("PAPI_register_clock");
  
  int ierr;
  
  cClockFuncs funcs;
  funcs.name    = clock_name;
  funcs.n_vals  = num_clock_vals;
  funcs.create  = clock_create;
  funcs.destroy = clock_destroy;
  funcs.start   = clock_start;
  funcs.stop    = clock_stop;
  funcs.reset   = clock_reset;
  funcs.get     = clock_get;
  funcs.set     = clock_set;
  funcs.seconds = NULL;
  ierr = CCTK_ClockRegister(clock_name, &funcs);
  assert(ierr>=0);
}
