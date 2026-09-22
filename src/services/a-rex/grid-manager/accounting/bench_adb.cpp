#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <cstdlib>
#include <iostream>
#include <sstream>
#include <sys/time.h>

#include "AccountingDBSQLite.h"
#include "AAR.h"

// Writes a number of job records into a fresh accounting database and reports
// how long it took. Used to compare the cost of the journalling mode the
// database is opened with; a single job record is several transactions, so
// the per-transaction cost is what this measures.
//
//   bench_adb <database path> <number of records>

static double now_seconds() {
  struct timeval tv;
  gettimeofday(&tv, NULL);
  return tv.tv_sec + tv.tv_usec / 1000000.0;
}

int main(int argc, char **argv) {
  if (argc < 3) {
    std::cerr << "usage: " << argv[0] << " <database> <records>" << std::endl;
    return EXIT_FAILURE;
  }
  std::string dbpath(argv[1]);
  int records = atoi(argv[2]);
  if (records <= 0) {
    std::cerr << "records must be positive" << std::endl;
    return EXIT_FAILURE;
  }

  Arc::LogStream logcerr(std::cerr);
  Arc::Logger::getRootLogger().addDestination(logcerr);
  Arc::Logger::getRootLogger().setThreshold(Arc::ERROR);

  ARex::AccountingDBSQLite adb(dbpath);
  if (!adb.IsValid()) {
    std::cerr << "Database connection was not successful" << std::endl;
    return EXIT_FAILURE;
  }

  double const start = now_seconds();
  for (int n = 0; n < records; ++n) {
    std::ostringstream id;
    id << "benchjob" << n;
    ARex::AAR aar;
    aar.jobid = id.str();
    aar.endpoint = { "org.nordugrid.arcrest", "https://ce3.example.org:443/arex/" };
    aar.queue = "eddie";
    aar.userdn = "/DC=org/DC=example/CN=bench";
    aar.wlcgvo = "dteam";
    aar.status = "completed";
    aar.submittime = Arc::Time();
    aar.endtime = Arc::Time();
    // A realistic record carries rows in the side tables too, each of which
    // is written as its own transaction.
    aar.authtokenattrs.push_back(ARex::aar_authtoken_t("vomsfqan", "/dteam"));
    aar.rtes.push_back("ENV/PROXY");
    aar.rtes.push_back("ENV/RTE");
    aar.extrainfo.insert(std::pair<std::string,std::string>("jobname", "bench"));
    aar.jobevents.push_back(ARex::aar_jobevent_t("ACCEPTED", Arc::Time()));
    aar.jobevents.push_back(ARex::aar_jobevent_t("FINISHED", Arc::Time()));
    if (!adb.createAAR(aar)) {
      std::cerr << "Failed writing record " << n << std::endl;
      return EXIT_FAILURE;
    }
  }
  double const elapsed = now_seconds() - start;

  std::cout.setf(std::ios::fixed);
  std::cout.precision(3);
  std::cout << "records=" << records
            << " seconds=" << elapsed
            << " records_per_second=" << (records / elapsed)
            << std::endl;
  return EXIT_SUCCESS;
}
