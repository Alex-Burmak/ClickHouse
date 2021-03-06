#include <Poco/String.h>

#include <Interpreters/TranslateQualifiedNamesVisitor.h>
#include <Interpreters/IdentifierSemantic.h>
#include <Interpreters/AsteriskSemantic.h>

#include <Common/typeid_cast.h>
#include <Core/Names.h>

#include <Parsers/ASTIdentifier.h>
#include <Parsers/ASTAsterisk.h>
#include <Parsers/ASTQualifiedAsterisk.h>
#include <Parsers/ASTSelectQuery.h>
#include <Parsers/ASTSelectWithUnionQuery.h>
#include <Parsers/ASTTablesInSelectQuery.h>
#include <Parsers/ASTExpressionList.h>
#include <Parsers/ASTLiteral.h>
#include <Parsers/ASTFunction.h>
#include <Parsers/ASTColumnsMatcher.h>


namespace DB
{

namespace ErrorCodes
{
    extern const int UNKNOWN_IDENTIFIER;
    extern const int UNKNOWN_ELEMENT_IN_AST;
    extern const int LOGICAL_ERROR;
}

bool TranslateQualifiedNamesMatcher::needChildVisit(ASTPtr & node, const ASTPtr & child)
{
    /// Do not go to FROM, JOIN, subqueries.
    if (child->as<ASTTableExpression>() || child->as<ASTSelectWithUnionQuery>())
        return false;

    /// Processed nodes. Do not go into children.
    if (node->as<ASTQualifiedAsterisk>() || node->as<ASTTableJoin>())
        return false;

    /// ASTSelectQuery + others
    return true;
}

void TranslateQualifiedNamesMatcher::visit(ASTPtr & ast, Data & data)
{
    if (auto * t = ast->as<ASTIdentifier>())
        visit(*t, ast, data);
    if (auto * t = ast->as<ASTTableJoin>())
        visit(*t, ast, data);
    if (auto * t = ast->as<ASTSelectQuery>())
        visit(*t, ast, data);
    if (auto * node = ast->as<ASTExpressionList>())
        visit(*node, ast, data);
    if (auto * node = ast->as<ASTFunction>())
        visit(*node, ast, data);
}

void TranslateQualifiedNamesMatcher::visit(ASTIdentifier & identifier, ASTPtr &, Data & data)
{
    if (IdentifierSemantic::getColumnName(identifier))
    {
        String short_name = identifier.shortName();
        size_t table_pos = 0;
        bool allow_ambiguous = data.join_using_columns.count(short_name);
        if (IdentifierSemantic::chooseTable(identifier, data.tables, table_pos, allow_ambiguous))
        {
            IdentifierSemantic::setMembership(identifier, table_pos);

            /// In case if column from the joined table are in source columns, change it's name to qualified.
            auto & table = data.tables[table_pos].first;
            if (table_pos && data.hasColumn(short_name))
                IdentifierSemantic::setColumnLongName(identifier, table);
            else
                IdentifierSemantic::setColumnShortName(identifier, table);
        }
    }
}

/// As special case, treat count(*) as count(), not as count(list of all columns).
void TranslateQualifiedNamesMatcher::visit(ASTFunction & node, const ASTPtr &, Data &)
{
    ASTPtr & func_arguments = node.arguments;

    String func_name_lowercase = Poco::toLower(node.name);
    if (func_name_lowercase == "count" &&
        func_arguments->children.size() == 1 &&
        func_arguments->children[0]->as<ASTAsterisk>())
        func_arguments->children.clear();
}

void TranslateQualifiedNamesMatcher::visit(const ASTQualifiedAsterisk & , const ASTPtr & ast, Data & data)
{
    if (ast->children.size() != 1)
        throw Exception("Logical error: qualified asterisk must have exactly one child", ErrorCodes::LOGICAL_ERROR);

    auto & ident = ast->children[0];

    /// @note it could contain table alias as table name.
    DatabaseAndTableWithAlias db_and_table(ident);

    for (const auto & known_table : data.tables)
        if (db_and_table.satisfies(known_table.first, true))
            return;

    throw Exception("Unknown qualified identifier: " + ident->getAliasOrColumnName(), ErrorCodes::UNKNOWN_IDENTIFIER);
}

void TranslateQualifiedNamesMatcher::visit(ASTTableJoin & join, const ASTPtr & , Data & data)
{
    if (join.using_expression_list)
        Visitor(data).visit(join.using_expression_list);
    else if (join.on_expression)
        Visitor(data).visit(join.on_expression);
}

void TranslateQualifiedNamesMatcher::visit(ASTSelectQuery & select, const ASTPtr & , Data & data)
{
    if (auto join = select.join())
        extractJoinUsingColumns(join->table_join, data);

    /// If the WHERE clause or HAVING consists of a single qualified column, the reference must be translated not only in children,
    /// but also in where_expression and having_expression.
    if (select.prewhere())
        Visitor(data).visit(select.refPrewhere());
    if (select.where())
        Visitor(data).visit(select.refWhere());
    if (select.having())
        Visitor(data).visit(select.refHaving());
}

static void addIdentifier(ASTs & nodes, const DatabaseAndTableWithAlias & table, const String & column_name,
                          AsteriskSemantic::RevertedAliasesPtr aliases)
{
    String table_name = table.getQualifiedNamePrefix(false);
    auto identifier = std::make_shared<ASTIdentifier>(std::vector<String>{table_name, column_name});

    bool added = false;
    if (aliases && aliases->count(identifier->name))
    {
        for (const String & alias : (*aliases)[identifier->name])
        {
            nodes.push_back(identifier->clone());
            nodes.back()->setAlias(alias);
            added = true;
        }
    }

    if (!added)
        nodes.emplace_back(identifier);
}

/// Replace *, alias.*, database.table.* with a list of columns.
void TranslateQualifiedNamesMatcher::visit(ASTExpressionList & node, const ASTPtr &, Data & data)
{
    const auto & tables_with_columns = data.tables;

    ASTs old_children;
    if (data.processAsterisks())
    {
        bool has_asterisk = false;
        for (const auto & child : node.children)
        {
            if (child->as<ASTAsterisk>() || child->as<ASTColumnsMatcher>())
            {
                if (tables_with_columns.empty())
                    throw Exception("An asterisk cannot be replaced with empty columns.", ErrorCodes::LOGICAL_ERROR);
                has_asterisk = true;
                break;
            }
            else if (const auto * qa = child->as<ASTQualifiedAsterisk>())
            {
                visit(*qa, child, data); /// check if it's OK before rewrite
                has_asterisk = true;
                break;
            }
        }

        if (has_asterisk)
        {
            old_children.swap(node.children);
            node.children.reserve(old_children.size());
        }
    }

    for (const auto & child : old_children)
    {
        if (const auto * asterisk = child->as<ASTAsterisk>())
        {
            bool first_table = true;
            for (const auto & [table, table_columns] : tables_with_columns)
            {
                for (const auto & column_name : table_columns)
                {
                    if (first_table || !data.join_using_columns.count(column_name))
                    {
                        addIdentifier(node.children, table, column_name, AsteriskSemantic::getAliases(*asterisk));
                    }
                }

                first_table = false;
            }
        }
        else if (const auto * asterisk_pattern = child->as<ASTColumnsMatcher>())
        {
            bool first_table = true;
            for (const auto & [table, table_columns] : tables_with_columns)
            {
                for (const auto & column_name : table_columns)
                {
                    if (asterisk_pattern->isColumnMatching(column_name) && (first_table || !data.join_using_columns.count(column_name)))
                    {
                        addIdentifier(node.children, table, column_name, AsteriskSemantic::getAliases(*asterisk_pattern));
                    }
                }

                first_table = false;
            }
        }
        else if (const auto * qualified_asterisk = child->as<ASTQualifiedAsterisk>())
        {
            DatabaseAndTableWithAlias ident_db_and_name(qualified_asterisk->children[0]);

            for (const auto & [table, table_columns] : tables_with_columns)
            {
                if (ident_db_and_name.satisfies(table, true))
                {
                    for (const auto & column_name : table_columns)
                    {
                        addIdentifier(node.children, table, column_name, AsteriskSemantic::getAliases(*qualified_asterisk));
                    }
                    break;
                }
            }
        }
        else
            node.children.emplace_back(child);
    }
}

/// 'select * from a join b using id' should result one 'id' column
void TranslateQualifiedNamesMatcher::extractJoinUsingColumns(const ASTPtr ast, Data & data)
{
    const auto & table_join = ast->as<ASTTableJoin &>();

    if (table_join.using_expression_list)
    {
        const auto & keys = table_join.using_expression_list->as<ASTExpressionList &>();
        for (const auto & key : keys.children)
            if (auto opt_column = tryGetIdentifierName(key))
                data.join_using_columns.insert(*opt_column);
            else if (key->as<ASTLiteral>())
                data.join_using_columns.insert(key->getColumnName());
            else
            {
                String alias = key->tryGetAlias();
                if (alias.empty())
                    throw Exception("Logical error: expected identifier or alias, got: " + key->getID(), ErrorCodes::LOGICAL_ERROR);
                data.join_using_columns.insert(alias);
            }
    }
}

void RestoreQualifiedNamesData::visit(ASTIdentifier & identifier, ASTPtr & ast)
{
    if (IdentifierSemantic::getColumnName(identifier))
    {
        if (IdentifierSemantic::getMembership(identifier))
        {
            ast = identifier.clone();
            ast->as<ASTIdentifier>()->restoreCompoundName();
        }
    }
}

}
