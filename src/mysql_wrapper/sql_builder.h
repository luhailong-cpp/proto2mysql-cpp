#pragma once

#include "src/mysql_wrapper/message_table.h"
#include "src/mysql_wrapper/value_codec.h"

namespace pbmysql {

// WHERE, ORDER BY and SetColExpr are trusted SQL fragments. All values belong
// in args. Validation errors throw std::invalid_argument before any SQL runs.
struct Assign {
    std::string column;
    std::string expression;
    std::vector<SqlValue> args;
    bool numeric = false;
    bool insert_only = false;
};
Assign SetCol(std::string column, SqlValue value);
Assign AddCol(std::string column, SqlValue delta);
Assign SubCol(std::string column, SqlValue delta);
Assign SetColExpr(std::string column, std::string expression, std::vector<SqlValue> args = {});
Assign SetNew(std::string column);
Assign AddNew(std::string column);
Assign MinNew(std::string column);
Assign MaxNew(std::string column);
Assign SetNewIfZero(std::string column);
Assign KeepOld(std::string column);

class SQLBuilder {
  public:
    explicit SQLBuilder(const MessageTable &table) : table_(table) {}
    explicit SQLBuilder(const google::protobuf::Message &message, const TableOptions &options = {})
        : owned_(std::make_shared<MessageTable>(message, options)), table_(*owned_) {}
    const std::string &TableName() const { return table_.TableName(); }
    const MessageTable &Table() const { return table_; }
    std::string CreateTable() const { return table_.CreateTableSql(); }
    Statement PrimaryKeyWhere(const google::protobuf::Message &message) const;

    Statement Insert(const google::protobuf::Message &message) const;
    Statement InsertSetFields(const google::protobuf::Message &message) const;
    Statement InsertIgnore(const google::protobuf::Message &message) const;
    Statement InsertIgnoreSetFields(const google::protobuf::Message &message) const;
    Statement Replace(const google::protobuf::Message &message) const;
    Statement BatchInsert(const std::vector<const google::protobuf::Message *> &messages) const;
    Statement
    BatchInsertIgnore(const std::vector<const google::protobuf::Message *> &messages) const;
    Statement BatchReplace(const std::vector<const google::protobuf::Message *> &messages) const;
    Statement Upsert(const google::protobuf::Message &message,
                     const StringVector &columns = {}) const;
    Statement UpsertAdd(const google::protobuf::Message &message,
                        const StringVector &columns) const;
    Statement UpsertKeepOld(const google::protobuf::Message &message) const;
    Statement UpsertWith(const google::protobuf::Message &message,
                         const std::vector<Assign> &assigns) const;
    Statement BatchUpsert(const std::vector<const google::protobuf::Message *> &messages,
                          const StringVector &columns = {}) const;
    Statement BatchUpsertWith(const std::vector<const google::protobuf::Message *> &messages,
                              const std::vector<Assign> &assigns) const;

    Statement SelectByPK(const google::protobuf::Message &message) const;
    Statement SelectByPKForUpdate(const google::protobuf::Message &message) const;
    Statement SelectWhere(const std::string &where = {}, const std::vector<SqlValue> &args = {},
                          const QueryOptions &options = {}) const;
    Statement SelectColumns(const StringVector &columns, const std::string &where = {},
                            const std::vector<SqlValue> &args = {},
                            const QueryOptions &options = {}) const;
    Statement SelectByKVIn(const std::string &column, const std::vector<SqlValue> &values,
                           const QueryOptions &options = {}) const;
    Statement SelectByPKIn(const std::vector<SqlValue> &values,
                           const QueryOptions &options = {}) const;
    Statement Count(const std::string &where = {}, const std::vector<SqlValue> &args = {}) const;
    Statement Exists(const std::string &where = {}, const std::vector<SqlValue> &args = {}) const;
    Statement ExistsByPKForUpdate(const google::protobuf::Message &message) const;

    Statement UpdateByPK(const google::protobuf::Message &message) const;
    Statement UpdateByPKIf(const google::protobuf::Message &message, const std::string &guard,
                           const std::vector<SqlValue> &args = {}) const;
    Statement UpdateFieldsByPK(const google::protobuf::Message &message,
                               const StringVector &columns) const;
    Statement UpdateWhere(const google::protobuf::Message &message, const std::string &where,
                          const std::vector<SqlValue> &args = {}) const;
    Statement UpdateAssignsByPK(const google::protobuf::Message &message,
                                const std::vector<Assign> &assigns) const;
    Statement UpdateAssignsWhere(const std::vector<Assign> &assigns, const std::string &where,
                                 const std::vector<SqlValue> &args = {}) const;
    Statement IncrByPK(const google::protobuf::Message &message, const std::string &column,
                       int64_t delta) const;
    Statement DecrByPKIfEnough(const google::protobuf::Message &message, const std::string &column,
                               int64_t delta) const;

    Statement DeleteByPK(const google::protobuf::Message &message) const;
    Statement DeleteByPKIf(const google::protobuf::Message &message, const std::string &guard,
                           const std::vector<SqlValue> &args = {}) const;
    Statement DeleteWhere(const std::string &where, const std::vector<SqlValue> &args = {}) const;
    Statement DeleteWhereLimit(const std::string &where, const std::vector<SqlValue> &args,
                               const std::string &order_by, int limit) const;
    Statement DeleteByKVIn(const std::string &column, const std::vector<SqlValue> &values) const;
    Statement DeleteByPKIn(const std::vector<SqlValue> &values) const;

  private:
    const google::protobuf::FieldDescriptor *Field(const std::string &column) const;
    StringVector Columns(bool exclude_primary = false) const;
    StringVector SetFields(const google::protobuf::Message &message, bool exclude_primary) const;
    Statement InsertColumns(const std::vector<const google::protobuf::Message *> &messages,
                            const StringVector &columns, bool replace) const;
    Statement BuildAssigns(const std::vector<Assign> &assigns, bool upsert) const;
    Statement AppendDuplicate(Statement insert, const std::vector<Assign> &assigns) const;
    Statement DuplicateNoop(Statement insert) const;
    Statement InWhere(const std::string &column, const std::vector<SqlValue> &values) const;
    SqlValue ComparisonValue(const std::string &column, const SqlValue &value) const;
    std::string SinglePK() const;
    std::shared_ptr<const MessageTable> owned_;
    const MessageTable &table_;
};

} // namespace pbmysql
