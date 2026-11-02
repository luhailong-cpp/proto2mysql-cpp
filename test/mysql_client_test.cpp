#include <gtest/gtest.h>

#include "src/mysql_wrapper/database.h"

#include "common_proto/mysql_database_test.pb.h"
#include "common_proto/mysql_database_table.pb.h"

using pbmysql::Database;
using pbmysql::DatabasePtr;

DatabasePtr query_database;
database_server_db query_database_param;
std::string main_account("lu hailong");
std::string main_password("lu yaun");
std::string main_where_case = std::string("account = '") + main_account + std::string("'");

TEST(DatabaseTest, ConnectMysql)
{
    Database client;
    client.Connect(query_database_param);
    client.RegisterTable(account_database_one_test::default_instance());
    client.SetDbName("game");
    client.SyncAllTables();
}

TEST(DatabaseTest, SaveAndFindOne)
{
    account_database_one_test save_message;
    save_message.set_account(main_account);
    save_message.set_password(main_password);
    query_database->Save(save_message);

    account_database_one_test load_message;
    load_message.set_account(main_account);
    EXPECT_TRUE(query_database->FindOneByPrimaryKey(load_message));
    EXPECT_EQ(save_message.account(), load_message.account());
    EXPECT_EQ(save_message.password(), load_message.password());

    account_database_one_test load_message_where;
    EXPECT_TRUE(query_database->FindOneByWhere(load_message_where, main_where_case));
    EXPECT_EQ(save_message.account(), load_message_where.account());
    EXPECT_EQ(save_message.password(), load_message_where.password());
}

TEST(DatabaseTest, BatchSaveAndFindAll)
{
    account_database_all_test save_message;
    auto* first = save_message.mutable_account_password()->Add();
    first->set_account("lu hailong1");
    first->set_password("luyuan ");
    auto* second = save_message.mutable_account_password()->Add();
    second->set_account("luh ailong1");
    second->set_password("lu yuan ");
    query_database->BatchSave<account_database_one_test>(save_message);

    account_database_all_test load_message;
    query_database->FindAll<account_database_one_test>(load_message);
    EXPECT_GE(load_message.account_password_size(), 2);
}

TEST(DatabaseTest, Delete)
{
    account_database_one_test save_message;
    save_message.set_account(main_account);
    save_message.set_password(main_account);
    query_database->Save(save_message);

    account_database_one_test delete_message;
    delete_message.set_account(main_account);
    query_database->Delete(delete_message);

    account_database_one_test load_message_where;
    EXPECT_FALSE(query_database->FindOneByWhere(load_message_where, main_where_case));
    EXPECT_TRUE(load_message_where.account().empty());
    EXPECT_TRUE(load_message_where.password().empty());
}

TEST(DatabaseTest, FindAllByWhere)
{
    account_database_one_test row;
    row.set_account("1");
    query_database->Save(row);
    row.set_account("2");
    query_database->Save(row);

    account_database_all_test load_message;
    query_database->FindAllByWhere<account_database_one_test>(load_message, "account = '1' or account = '2'");
    EXPECT_EQ(2, load_message.account_password_size());
}

TEST(DatabaseTest, CountAndExists)
{
    account_database_one_test row;
    row.set_account("count-key");
    row.set_password("p");
    query_database->Save(row);

    EXPECT_GE(query_database->Count(row), 1);
    EXPECT_TRUE(query_database->Exists(row, "account = 'count-key'"));

    account_database_one_test probe;
    probe.set_account("count-key");
    EXPECT_TRUE(query_database->ExistsByPrimaryKey(probe));
}

TEST(DatabaseTest, Transaction)
{
    bool committed = query_database->Transaction([](Database& tx) {
        account_database_one_test row;
        row.set_account("tx-key");
        row.set_password("tx-pass");
        tx.Save(row);
        return true;
    });
    EXPECT_TRUE(committed);

    account_database_one_test probe;
    probe.set_account("tx-key");
    EXPECT_TRUE(query_database->FindOneByPrimaryKey(probe));
    EXPECT_EQ("tx-pass", probe.password());
}

int main(int argc, char** argv)
{
    query_database_param.set_db_host("127.0.0.1");
    query_database_param.set_db_user("root");
    query_database_param.set_db_password("lhl616586");
    query_database_param.set_db_port(MYSQL_PORT);
    query_database_param.set_db_dbname("game");

    query_database = std::make_shared<Database>();
    query_database->Connect(query_database_param);
    query_database->SetDbName("game");
    query_database->RegisterTable(account_database_one_test::default_instance());

    std::string drop_table = "drop table if exists " +
        query_database->tables().begin()->second.TableName();
    query_database->Execute(drop_table);

    query_database->SyncAllTables();

    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}