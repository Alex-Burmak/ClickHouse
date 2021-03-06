#pragma once
#include <iostream>

#include <Client/Connection.h>


namespace DB
{

// Use template to disable implicit casting for certain overloaded types such as Field, which leads
// to overload resolution ambiguity.
class Field;
template <typename T, typename U = std::enable_if_t<std::is_same_v<T, Field>>>
std::ostream & operator<<(std::ostream & stream, const T & what);

class IBlockInputStream;
std::ostream & operator<<(std::ostream & stream, const IBlockInputStream & what);

struct NameAndTypePair;
std::ostream & operator<<(std::ostream & stream, const NameAndTypePair & what);

class IDataType;
std::ostream & operator<<(std::ostream & stream, const IDataType & what);

class IStorage;
std::ostream & operator<<(std::ostream & stream, const IStorage & what);

class TableStructureReadLock;
std::ostream & operator<<(std::ostream & stream, const TableStructureReadLock & what);

class IFunctionBase;
std::ostream & operator<<(std::ostream & stream, const IFunctionBase & what);

class Block;
std::ostream & operator<<(std::ostream & stream, const Block & what);

struct ColumnWithTypeAndName;
std::ostream & operator<<(std::ostream & stream, const ColumnWithTypeAndName & what);

class IColumn;
std::ostream & operator<<(std::ostream & stream, const IColumn & what);

std::ostream & operator<<(std::ostream & stream, const Connection::Packet & what);

struct ExpressionAction;
std::ostream & operator<<(std::ostream & stream, const ExpressionAction & what);

class ExpressionActions;
std::ostream & operator<<(std::ostream & stream, const ExpressionActions & what);

struct SyntaxAnalyzerResult;
std::ostream & operator<<(std::ostream & stream, const SyntaxAnalyzerResult & what);
}

/// some operator<< should be declared before operator<<(... std::shared_ptr<>)
#include <common/iostream_debug_helpers.h>
