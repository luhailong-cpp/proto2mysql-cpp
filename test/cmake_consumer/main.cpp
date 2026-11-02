#include "src/mysql_wrapper/database.h"
#include <cstdlib>
#include <iostream>
int main() {
    // No generated header or message type is named by this application.
    pbmysql::Database database;
    database.RegisterAllTables();
    if (database.tables().count("helper_probe.UnreferencedRow") != 1) {
        std::cerr << "automatic proto registration was discarded by the linker\n";
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
