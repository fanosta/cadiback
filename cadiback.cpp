// clang-format off

#define VERSION "0.1.4"

static const char * usage =

"usage: cadiback [ <option> ... ] [ <dimacs> ]\n"
"\n"
"where '<option>' is one of the following\n"
"\n"
"  -h            print this command line option summary\n"
"  -l            extensive logging for debugging\n"
"  -n            do not print backbone \n"
"  -q            disable all messages\n"
"  -r            report what the solver is doing\n"
"  -s            always print full statistics (not only with '-v')\n"
"  -v            increase verbosity\n"
"                (SAT solver verbosity is increased with two '-v')\n"
"\n"
"  --one-by-one  try each candidate one-by-one (do not use 'constrain')\n"
"  --version     print version and exit\n"
"\n"
"and '<dimacs>' is a SAT instances for which the backbone literals are\n"
"determined and then printed (unless '-n' is specified).  If no input\n"
"file is given the formula is read from '<stdin>'.\n"

;

// clang-format on

#include <cassert>
#include <climits>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "cadical.hpp"
#include "resources.hpp"
#include "signal.hpp"

// Verbosity level: -1=quiet, 0=default, 1=verbose, INT_MAX=logging.

static int verbosity;

// Print backbones by default. Otherwise only produce statistics.
//
bool print = true;

// Disable by default  printing those 'c <character> ...' lines
// in the solver.  If enabled is useful to see what is going on.
//
bool report = false;

bool always_print_statistics;

// Try each candidate after each other with a single assumption, i.e., do
// not use the 'constrain' optimization.
//
bool one_by_one;

static int vars;      // The number of variables in the CNF.
static int *backbone; // The backbone candidates (if non-zero).

static size_t backbones;     // Number of backbones found.
static size_t dropped;       // Number of non-backbones found.
static size_t sat_calls;     // Calls with result SAT to SAT solver.
static size_t unsat_calls;   // Calls with result UNSAT to SAT solver.
static size_t unknown_calls; // Interrupted solver calls.
static size_t calls;         // Calls to SAT solver.

static double first_time, sat_time, unsat_time, solving_time, unknown_time;
static double satmax_time, unsatmax_time;
static volatile double started = -1;

static void die (const char *, ...) __attribute__ ((format (printf, 1, 2)));
static void msg (const char *, ...) __attribute__ ((format (printf, 1, 2)));

static void msg (const char *fmt, ...) {
  if (verbosity < 0)
    return;
  fputs ("c ", stdout);
  va_list ap;
  va_start (ap, fmt);
  vprintf (fmt, ap);
  va_end (ap);
  fputc ('\n', stdout);
  fflush (stdout);
}

static void line () {
  if (verbosity < 0)
    return;
  fputs ("c\n", stdout);
  fflush (stdout);
}

static void die (const char *fmt, ...) {
  fputs ("cadiback: error: ", stderr);
  va_list ap;
  va_start (ap, fmt);
  vfprintf (stderr, fmt, ap);
  va_end (ap);
  fputc ('\n', stderr);
  exit (1);
}

static void dbg (const char *fmt, ...) {
  if (verbosity < INT_MAX)
    return;
  fputs ("c LOGGING ", stdout);
  va_list ap;
  va_start (ap, fmt);
  vprintf (fmt, ap);
  va_end (ap);
  fputc ('\n', stdout);
  fflush (stdout);
}

static CaDiCaL::Solver *solver;

static double average (double a, double b) { return b ? a / b : 0; }
static double percent (double a, double b) { return average (100 * a, b); }

static double time () { return CaDiCaL::absolute_process_time (); }

static void statistics () {
  if (verbosity < 0)
    return;
  if (started >= 0) {
    double end = time ();
    double delta = end - started;
    started = -1;
    unknown_time += delta;
    solving_time += delta;
    unknown_calls++;
  }
  printf ("c\n");
  printf ("c --- [ backbone statistics ] ");
  printf ("------------------------------------------------\n");
  printf ("c\n");
  printf ("c found %zu backbones %.0f%% (%zu dropped %.0f%%)\n", backbones,
          percent (backbones, vars), dropped, percent (dropped, vars));
  printf ("c called SAT solver %zu times (%zu SAT, %zu UNSAT)\n", calls,
          sat_calls, unsat_calls);
  printf ("c\n");
  if (always_print_statistics || verbosity > 0 || first_time)
    printf ("c   %10.2f %6.2f %% first\n", first_time,
            percent (first_time, solving_time));
  if (verbosity > 0 || sat_time)
    printf ("c   %10.2f %6.2f %% sat\n", sat_time,
            percent (sat_time, solving_time));
  if (verbosity > 0 || satmax_time)
    printf ("c   %10.2f %6.2f %% satmax\n", satmax_time,
            percent (satmax_time, solving_time));
  if (verbosity > 0 || unsat_time)
    printf ("c   %10.2f %6.2f %% unsat\n", unsat_time,
            percent (unsat_time, solving_time));
  if (verbosity > 0 || unsatmax_time)
    printf ("c   %10.2f %6.2f %% unsatmax\n", unsatmax_time,
            percent (unsatmax_time, solving_time));
  if (verbosity > 0 || unknown_time)
    printf ("c   %10.2f %6.2f %% unknown\n", unknown_time,
            percent (unknown_time, solving_time));
  printf ("c ---------------------------------\n");
  printf ("c   %10.2f 100.00 %% solving\n", solving_time);
  printf ("c\n");
  printf ("c\n");
  fflush (stdout);
  if (!solver)
    return;
  if (always_print_statistics || verbosity > 0)
    solver->statistics ();
  solver->resources ();
}

class CadiBackSignalHandler : public CaDiCaL::Handler {
  virtual void catch_signal (int sig) {
    if (verbosity < 0)
      return;
    printf ("c caught signal %d\n", sig);
    statistics ();
  }
};

static int solve () {
  assert (solver);
  started = time ();
  calls++;
  int res = solver->solve ();
  if (res == 10) {
    sat_calls++;
  } else {
    assert (res == 20);
    unsat_calls++;
  }
  double end = time ();
  double delta = end - started;
  started = -1;
  if (calls == 1)
    first_time = delta;
  if (res == 10) {
    sat_time += delta;
    if (delta > satmax_time)
      satmax_time = delta;
  } else {
    unsat_time += delta;
    if (delta > unsatmax_time)
      unsatmax_time = delta;
  }
  solving_time += delta;
  return res;
}

static void drop_candidate (int idx) {
  int lit = backbone[idx];
  if (!lit)
    return;
  int val = solver->val (idx);
  if (lit == val)
    return;
  assert (lit == -val);
  dbg ("model also satisfies negation %d "
       "of backbone candidate %d thus dropping %d",
       -lit, lit, lit);
  backbone[idx] = 0;
  dropped++;
}

static void drop_candidates (int start) {
  for (int idx = start; idx <= vars; idx++)
    drop_candidate (idx);
}

static void backbone_variable (int idx) {
  int lit = backbone[idx];
  if (!lit)
    return;
  if (print) {
    printf ("b %d\n", lit);
    fflush (stdout);
  }
  backbones++;
}

static void backbone_variables (int start) {
  for (int idx = start; idx <= vars; idx++)
    backbone_variable (idx);
}

int main (int argc, char **argv) {

  const char *path = 0; // The path to the input file.

  for (int i = 1; i != argc; i++) {
    const char *arg = argv[i];
    if (!strcmp (arg, "-h")) {
      fputs (usage, stdout);
      exit (0);
    } else if (!strcmp (arg, "--version")) {
      fputs (VERSION, stdout);
      fputc ('\n', stdout);
      exit (0);
    } else if (!strcmp (arg, "-l")) {
      verbosity = INT_MAX;
    } else if (!strcmp (arg, "-n")) {
      print = false;
    } else if (!strcmp (arg, "-q")) {
      verbosity = -1;
    } else if (!strcmp (arg, "-r")) {
      report = true;
    } else if (!strcmp (arg, "-s")) {
      always_print_statistics = true;
    } else if (!strcmp (arg, "-v")) {
      if (verbosity < 0)
        verbosity = 1;
      else if (verbosity < INT_MAX)
        verbosity++;
    } else if (!strcmp (arg, "--one-by-one")) {
      one_by_one = true;
    } else if (*arg == '-')
      die ("invalid option '%s' (try '-h')", arg);
    else if (path)
      die ("multiple file arguments '%s' and '%s'", path, arg);
    else
      path = arg;
  }

  msg ("CaDiBack BackBone Analyzer");
  msg ("Copyright (c) 2023 Armin Biere University of Freiburg");
  msg ("Version " VERSION " CaDiCaL %s", CaDiCaL::Solver::version ());
  line ();

  solver = new CaDiCaL::Solver ();

  if (verbosity < 0)
    solver->set ("quiet", 1);
  else if (verbosity > 0)
    solver->set ("verbose", verbosity - 1);
  if (report || verbosity > 1)
    solver->set ("report", 1);

  int res;
  {
    CadiBackSignalHandler handler;
    CaDiCaL::Signal::set (&handler);
    dbg ("initialized solver");
    {
      const char *err;
      if (path) {
        msg ("reading from '%s'", path);
        err = solver->read_dimacs (path, vars);
      } else {
        msg ("reading from '<stdin>");
        err = solver->read_dimacs (stdin, "<stdin>", vars);
      }
      if (err)
        die ("%s", err);
      if (vars == INT_MAX) {
        die ("can not support 'INT_MAX == %d' variables", vars);
        // Otherwise 'vars + 1' as well as the idiom 'idx <= vars' does not
        // work and for simplicity we force having less variables here.
      }
    }
    msg ("found %d variables", vars);
    line ();
    msg ("starting solving after %.2f seconds", time ());
    res = solve ();
    assert (res == 10 || res == 20);
    if (res == 10) {
      msg ("solver determined first model after %.2f seconds", time ());
      line ();
      backbone = new int[vars + 1];
      if (!backbone)
        die ("out-of-memory allocating backbone array");
      for (int idx = 1; idx <= vars; idx++)
        backbone[idx] = solver->val (idx);
      for (int idx = 1; idx <= vars; idx++) {
        int lit = backbone[idx];
        if (!backbone[idx]) {
          dbg ("skipping dropped non-backbone variable %d", idx);
          continue;
        }
	if (!one_by_one) {
          int assumed = 0;
          for (int other = idx + 1; other <= vars; other++) {
            int lit_other = backbone[other];
            if (!lit_other)
              continue;
            solver->constrain (-lit_other);
            assumed++;
          }
          if (assumed++) {
	    solver->constrain (-lit);
	    solver->constrain (0);
	    dbg ("assuming all %d remaining backbone candidates "
	         "starting with %d", assumed, lit);
	    int tmp = solve ();
	    if (tmp == 10) {
	      dbg ("constraining all backbones candidates starting at %d "
	           "all-at-once produced model", lit);
	      drop_candidates (idx);
	    } else {
	      assert (tmp == 20);
	      msg ("all %d remaining candidates starting at %d "
	           "shown to be backbones in one call", assumed, lit);
	      backbone_variables (idx);
	      break;
	    }
          } else
            dbg ("no other literal besides %d remains a backbone candidate",
                 lit);
        }
        dbg ("assuming negation %d of backbone candidate %d", -lit, lit);
        solver->assume (-lit);
        int tmp = solve ();
        if (tmp == 10) {
          dbg ("found model satisfying single assumed "
               "negation %d of backbone candidate %d",
               -lit, lit);
	  drop_candidates (idx);
        } else {
          assert (tmp == 20);
          dbg ("no model with %d thus found backbone literal %d", -lit,
               lit);
	  backbone_variable (idx);
        }
      }
      if (print) {
        printf ("b 0\n");
        fflush (stdout);
      }
      line ();
      printf ("s SATISFIABLE\n");
      fflush (stdout);
      delete[] backbone;
    } else {
      assert (res == 20);
      printf ("s UNSATISFIABLE\n");
    }
    statistics ();
    dbg ("deleting solver");
    CaDiCaL::Signal::reset ();
  }
  delete solver;
  line ();
  msg ("exit %d", res);
  return res;
}
