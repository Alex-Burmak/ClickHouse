#include <Formats/RowInputStreamWithDiagnosticInfo.h>
#include <Formats/verbosePrintString.h>
#include <IO/Operators.h>
#include <IO/WriteBufferFromString.h>


namespace DB
{

namespace ErrorCodes
{
    extern const int LOGICAL_ERROR;
}

DB::RowInputStreamWithDiagnosticInfo::RowInputStreamWithDiagnosticInfo(ReadBuffer & istr_, const Block & header_)
    : istr(istr_), header(header_)
{
}

void DB::RowInputStreamWithDiagnosticInfo::updateDiagnosticInfo()
{
    ++row_num;

    bytes_read_at_start_of_buffer_on_prev_row = bytes_read_at_start_of_buffer_on_current_row;
    bytes_read_at_start_of_buffer_on_current_row = istr.count() - istr.offset();

    offset_of_prev_row = offset_of_current_row;
    offset_of_current_row = istr.offset();
}

String DB::RowInputStreamWithDiagnosticInfo::getDiagnosticInfo()
{
    if (istr.eof())        /// Buffer has gone, cannot extract information about what has been parsed.
        return {};

    WriteBufferFromOwnString out;

    MutableColumns columns = header.cloneEmptyColumns();

    /// It is possible to display detailed diagnostics only if the last and next to last rows are still in the read buffer.
    size_t bytes_read_at_start_of_buffer = istr.count() - istr.offset();
    if (bytes_read_at_start_of_buffer != bytes_read_at_start_of_buffer_on_prev_row)
    {
        out << "Could not print diagnostic info because two last rows aren't in buffer (rare case)\n";
        return out.str();
    }

    max_length_of_column_name = 0;
    for (size_t i = 0; i < header.columns(); ++i)
        if (header.safeGetByPosition(i).name.size() > max_length_of_column_name)
            max_length_of_column_name = header.safeGetByPosition(i).name.size();

    max_length_of_data_type_name = 0;
    for (size_t i = 0; i < header.columns(); ++i)
        if (header.safeGetByPosition(i).type->getName().size() > max_length_of_data_type_name)
            max_length_of_data_type_name = header.safeGetByPosition(i).type->getName().size();

    /// Roll back the cursor to the beginning of the previous or current row and parse all over again. But now we derive detailed information.

    if (offset_of_prev_row <= istr.buffer().size())
    {
        istr.position() = istr.buffer().begin() + offset_of_prev_row;

        out << "\nRow " << (row_num - 1) << ":\n";
        if (!parseRowAndPrintDiagnosticInfo(columns, out))
            return out.str();
    }
    else
    {
        if (istr.buffer().size() < offset_of_current_row)
        {
            out << "Could not print diagnostic info because parsing of data hasn't started.\n";
            return out.str();
        }

        istr.position() = istr.buffer().begin() + offset_of_current_row;
    }

    out << "\nRow " << row_num << ":\n";
    parseRowAndPrintDiagnosticInfo(columns, out);
    out << "\n";

    return out.str();
}

bool RowInputStreamWithDiagnosticInfo::deserializeFieldAndPrintDiagnosticInfo(const String & col_name, const DataTypePtr & type,
                                                                              IColumn & column,
                                                                              WriteBuffer & out,
                                                                              size_t input_position)
{
    out << "Column " << input_position << ", " << std::string((input_position < 10 ? 2 : input_position < 100 ? 1 : 0), ' ')
        << "name: " << alignedName(col_name, max_length_of_column_name)
        << "type: " << alignedName(type->getName(), max_length_of_data_type_name);

    auto prev_position = istr.position();
    auto curr_position = istr.position();
    std::exception_ptr exception;

    try
    {
        tryDeserializeFiled(type, column, input_position, prev_position, curr_position);
    }
    catch (...)
    {
        exception = std::current_exception();
    }

    if (curr_position < prev_position)
        throw Exception("Logical error: parsing is non-deterministic.", ErrorCodes::LOGICAL_ERROR);

    if (isNativeNumber(type) || isDateOrDateTime(type))
    {
        /// An empty string instead of a value.
        if (curr_position == prev_position)
        {
            out << "ERROR: text ";
            verbosePrintString(prev_position, std::min(prev_position + 10, istr.buffer().end()), out);
            out << " is not like " << type->getName() << "\n";
            return false;
        }
    }

    out << "parsed text: ";
    verbosePrintString(prev_position, curr_position, out);

    if (exception)
    {
        if (type->getName() == "DateTime")
            out << "ERROR: DateTime must be in YYYY-MM-DD hh:mm:ss or NNNNNNNNNN (unix timestamp, exactly 10 digits) format.\n";
        else if (type->getName() == "Date")
            out << "ERROR: Date must be in YYYY-MM-DD format.\n";
        else
            out << "ERROR\n";
        return false;
    }

    out << "\n";

    if (type->haveMaximumSizeOfValue())
    {
        if (isGarbageAfterField(input_position, curr_position))
        {
            out << "ERROR: garbage after " << type->getName() << ": ";
            verbosePrintString(curr_position, std::min(curr_position + 10, istr.buffer().end()), out);
            out << "\n";

            if (type->getName() == "DateTime")
                out << "ERROR: DateTime must be in YYYY-MM-DD hh:mm:ss or NNNNNNNNNN (unix timestamp, exactly 10 digits) format.\n";
            else if (type->getName() == "Date")
                out << "ERROR: Date must be in YYYY-MM-DD format.\n";

            return false;
        }
    }

    return true;
}

String RowInputStreamWithDiagnosticInfo::alignedName(const String & name, size_t max_length) const
{
    size_t spaces_count = max_length >= name.size() ? max_length - name.size() : 0;
    return name + ", " + std::string(spaces_count, ' ');
}

}