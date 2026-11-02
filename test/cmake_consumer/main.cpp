#include "src/mysql_wrapper/database.h"
#include <iostream>
int main() {
    // No generated header or message type is named by this application.
    pbmysql::Database database;
    database.RegisterAllTables();
    if (database.tables().count("helper_probe.UnreferencedRow") != 1) {
        std::cerr << "automatic proto registration was discarded by the linker\n";
        return 1;
    }
    return 0;
}
