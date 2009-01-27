//! \file http.cpp Defines functions and data for handling MySQL queries
/***************************************************************************
* Copyright (C) 2007 Eddie Carle [eddie@mailforce.net]                     *
*                                                                          *
* This file is part of fastcgi++.                                          *
*                                                                          *
* fastcgi++ is free software: you can redistribute it and/or modify it     *
* under the terms of the GNU Lesser General Public License as  published   *
* by the Free Software Foundation, either version 3 of the License, or (at *
* your option) any later version.                                          *
*                                                                          *
* fastcgi++ is distributed in the hope that it will be useful, but WITHOUT *
* ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or    *
* FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public     *
* License for more details.                                                *
*                                                                          *
* You should have received a copy of the GNU Lesser General Public License *
* along with fastcgi++.  If not, see <http://www.gnu.org/licenses/>.       *
****************************************************************************/


#include <fastcgi++/mysql.hpp>
#include <utf8_codecvt.hpp>

void Fastcgipp::Sql::MySQL::Connection::connect(const char* host, const char* user, const char* passwd, const char* db, unsigned int port, const char* unix_socket, unsigned long client_flag, const char* const charset)
{
	if(!mysql_init(&connection))
		throw Exceptions::MySQL(&connection);

	if(!mysql_real_connect(&connection, host, user, passwd, db, port, unix_socket, client_flag))
		throw Exceptions::MySQL(&connection);

	if(mysql_set_character_set(&connection, charset))
		throw Exceptions::MySQL(&connection);
	
	if(!(foundRowsStatement = mysql_stmt_init(&connection)))
		throw Exceptions::MySQL(&connection);

	if(mysql_stmt_prepare(foundRowsStatement, "SELECT FOUND_ROWS()", 19))
		throw Exceptions::MySQL(foundRowsStatement);

	std::memset(&foundRowsBinding, 0, sizeof(MYSQL_BIND));
	foundRowsBinding.buffer_type = MYSQL_TYPE_LONGLONG;
	foundRowsBinding.is_unsigned = 1;
}

Fastcgipp::Sql::MySQL::Connection::~Connection()
{
	mysql_stmt_close(foundRowsStatement);
	mysql_close(&connection);
}

void Fastcgipp::Sql::MySQL::Connection::getFoundRows(unsigned long long* const& rows)
{
	if(mysql_stmt_bind_param(foundRowsStatement, 0))
		throw Exceptions::MySQL(foundRowsStatement);
	
	if(mysql_stmt_execute(foundRowsStatement))
		throw Exceptions::MySQL(foundRowsStatement);

	foundRowsBinding.buffer = rows;
	if(mysql_stmt_bind_result(foundRowsStatement, &foundRowsBinding))
		throw Exceptions::MySQL(foundRowsStatement);

	if(mysql_stmt_fetch(foundRowsStatement))
		throw Exceptions::MySQL(foundRowsStatement);
	mysql_stmt_free_result(foundRowsStatement);
	mysql_stmt_reset(foundRowsStatement);

}

void Fastcgipp::Sql::MySQL::Statement::init(const char* const& queryString, const size_t& queryLength, const Data::Set* const parameterSet, const Data::Set* const resultSet)
{
	stmt=mysql_stmt_init(&connection.connection);
	if(!stmt)
		throw Exceptions::MySQL(&connection.connection);

	if(mysql_stmt_prepare(stmt, queryString, queryLength))
		throw Exceptions::MySQL(stmt);

	if(parameterSet) buildBindings(stmt, *parameterSet, paramsConversions, paramsBindings);
	if(resultSet) buildBindings(stmt, *resultSet, resultsConversions, resultsBindings);
}

void Fastcgipp::Sql::MySQL::Statement::execute(Data::Set* const parameters, Data::SetContainerPar* const results, unsigned long long int* const insertId, unsigned long long int* const rows)
{
	boost::lock_guard<boost::mutex> executeLock(executeMutex);

	if(parameters)
	{
		bindBindings(stmt, *parameters, paramsConversions, paramsBindings);
		for(Data::Conversions::iterator it=paramsConversions.begin(); it!=paramsConversions.end(); ++it)
			it->second->convertParam();
	}

	if(mysql_stmt_bind_param(stmt, paramsBindings.get())!=0) throw Exceptions::MySQL(stmt);
	if(mysql_stmt_execute(stmt)!=0) throw Exceptions::MySQL(stmt);

	if(results)
	{
		Data::SetContainerPar& res=*results;
		Data::SetContainerPar::Cont::iterator it;

		bool keepLooping=true;
		while(keepLooping)
		{
			it=res.manufacture();
			bindBindings(stmt, *it->get(), resultsConversions, resultsBindings);
			if(mysql_stmt_bind_result(stmt, resultsBindings.get())!=0) throw Exceptions::MySQL(stmt);
			switch (mysql_stmt_fetch(stmt))
			{
			case 1:
				throw Exceptions::MySQL(stmt);
			case MYSQL_NO_DATA:
				res.trim();
				keepLooping=false;
				break;
			default:
				for(Data::Conversions::iterator it=resultsConversions.begin(); it!=resultsConversions.end(); ++it)
					it->second->convertResult();
			};
		}

		if(rows) connection.getFoundRows(rows);
	}
	else
	{
		if(rows) *rows = mysql_stmt_affected_rows(stmt);
		if(insertId) *insertId = mysql_stmt_insert_id(stmt);
	}

	mysql_stmt_free_result(stmt);
	mysql_stmt_reset(stmt);
}

void Fastcgipp::Sql::MySQL::Statement::buildBindings(MYSQL_STMT* const& stmt, const Fastcgipp::Sql::Data::Set& set, Fastcgipp::Sql::Data::Conversions& conversions, boost::scoped_array<MYSQL_BIND>& bindings)
{
	using namespace Data;

	const int& bindSize=set.numberOfSqlElements();
	bindings.reset(new MYSQL_BIND[bindSize]);

	std::memset(bindings.get(), 0, sizeof(MYSQL_BIND)*bindSize);

	for(int i=0; i<bindSize; ++i)
	{{
		Type type=set.getSqlType(i);

		// Handle NULL
		if(type>=U_TINY_N)
			type=Type(type-U_TINY_N);	// Make it the same type without the nullableness

		// Handle unsigned
		if(type<=U_BIGINT)
		{
			bindings[i].is_unsigned=1;
			type=Type(type+TINY);
		}

		// Start decoding values
		switch(type)
		{
			case TINY:
			{
				bindings[i].buffer_type=MYSQL_TYPE_TINY;
				break;
			}

			case SHORT:
			{
				bindings[i].buffer_type=MYSQL_TYPE_SHORT;
				break;
			}
			
			case INT:
			{
				bindings[i].buffer_type=MYSQL_TYPE_LONG;
				break;
			}

			case BIGINT:
			{
				bindings[i].buffer_type=MYSQL_TYPE_LONGLONG;
				break;
			}

			case FLOAT:
			{
				bindings[i].buffer_type=MYSQL_TYPE_FLOAT;
				break;
			}

			case DOUBLE:
			{
				bindings[i].buffer_type=MYSQL_TYPE_DOUBLE;
				break;
			}

			case DATE:
			{
				TypedConversion<Date>* conv = new TypedConversion<Date>;
				bindings[i].buffer = &conv->internal;
				bindings[i].buffer_type = MYSQL_TYPE_DATE;
				conversions[i].reset(conv);
				break;
			}

			case DATETIME:
			{
				TypedConversion<Datetime>* conv = new TypedConversion<Datetime>;
				bindings[i].buffer = &conv->internal;
				bindings[i].buffer_type = MYSQL_TYPE_DATETIME;
				conversions[i].reset(conv);
				break;
			}

			case TIME:
			{
				TypedConversion<Time>* conv = new TypedConversion<Time>;
				bindings[i].buffer = &conv->internal;
				bindings[i].buffer_type = MYSQL_TYPE_TIME;
				conversions[i].reset(conv);
				break;
			}

			case BLOB:
			{
				TypedConversion<Blob>* conv = new TypedConversion<Blob>(i, stmt, MYSQL_TYPE_BLOB, bindings[i].buffer);
				bindings[i].length = &conv->length;
				bindings[i].buffer_type = conv->bufferType;
				conversions[i].reset(conv);
				break;
			}

			case TEXT:
			{
				TypedConversion<Text>* conv = new TypedConversion<Text>(i, stmt, MYSQL_TYPE_STRING, bindings[i].buffer);
				bindings[i].length = &conv->length;
				bindings[i].buffer_type = conv->bufferType;
				conversions[i].reset(conv);
				break;
			}

			case WTEXT:
			{
				TypedConversion<Wtext>* conv = new TypedConversion<Wtext>(i, stmt, bindings[i].buffer);

				bindings[i].length = &conv->length;
				bindings[i].buffer_type = conv->bufferType;
				conversions[i].reset(conv);
				break;
			}

			case CHAR:
			case BINARY:
			{
				bindings[i].buffer_length = set.getSqlSize(i);
				bindings[i].buffer_type = type==CHAR?MYSQL_TYPE_STRING:MYSQL_TYPE_BLOB;
			}
		}
	}}
}

void Fastcgipp::Sql::MySQL::Statement::bindBindings(MYSQL_STMT* const& stmt, Data::Set& set, Data::Conversions& conversions, boost::scoped_array<MYSQL_BIND>& bindings)
{
	int bindSize=set.numberOfSqlElements();
	for(int i=0; i<bindSize; ++i)
	{{
		void* data=set.getSqlPtr(i);
		if(set.getSqlType(i) >= Data::U_TINY_N)
		{
			bindings[i].is_null = (my_bool*)&((Data::NullablePar*)data)->nullness;
			data = ((Data::NullablePar*)data)->getVoid();
		}

		Data::Conversions::iterator it=conversions.find(i);
		if(it==conversions.end())
			bindings[i].buffer=data;
		else
		{
			it->second->external=data;
			bindings[i].buffer=it->second->getPointer();
		}
	}}
}

void Fastcgipp::Sql::MySQL::TypedConversion<Fastcgipp::Sql::Data::Datetime>::convertResult()
{
	*(boost::posix_time::ptime*)external=boost::posix_time::ptime(boost::gregorian::date(internal.year, internal.month, internal.day), boost::posix_time::time_duration(internal.hour, internal.minute, internal.second));
}

void Fastcgipp::Sql::MySQL::TypedConversion<Fastcgipp::Sql::Data::Datetime>::convertParam()
{
	std::memset(&internal, 0, sizeof(MYSQL_TIME));
	internal.year = ((boost::posix_time::ptime*)external)->date().year();
	internal.month = ((boost::posix_time::ptime*)external)->date().month();
	internal.day = ((boost::posix_time::ptime*)external)->date().day();
	internal.hour = ((boost::posix_time::ptime*)external)->time_of_day().hours();
	internal.minute = ((boost::posix_time::ptime*)external)->time_of_day().minutes();
	internal.second = ((boost::posix_time::ptime*)external)->time_of_day().seconds();
}

void Fastcgipp::Sql::MySQL::TypedConversion<Fastcgipp::Sql::Data::Date>::convertResult()
{
	*(boost::gregorian::date*)external=boost::gregorian::date(internal.year, internal.month, internal.day);
}

void Fastcgipp::Sql::MySQL::TypedConversion<Fastcgipp::Sql::Data::Date>::convertParam()
{
	std::memset(&internal, 0, sizeof(MYSQL_TIME));
	internal.year = ((boost::gregorian::date*)external)->year();
	internal.month = ((boost::gregorian::date*)external)->month();
	internal.day = ((boost::gregorian::date*)external)->day();
}

void Fastcgipp::Sql::MySQL::TypedConversion<Fastcgipp::Sql::Data::Time>::convertResult()
{
	*(boost::posix_time::time_duration*)external = boost::posix_time::time_duration(internal.neg?internal.hour*-1:internal.hour, internal.minute, internal.second);
}

void Fastcgipp::Sql::MySQL::TypedConversion<Fastcgipp::Sql::Data::Time>::convertParam()
{
	std::memset(&internal, 0, sizeof(MYSQL_TIME));
	internal.hour = std::abs(((boost::posix_time::time_duration*)external)->hours());
	internal.minute = std::abs(((boost::posix_time::time_duration*)external)->minutes());
	internal.second = std::abs(((boost::posix_time::time_duration*)external)->seconds());
	internal.neg = ((boost::posix_time::time_duration*)external)->hours() < 0 ? 1:0;
}

template void Fastcgipp::Sql::MySQL::TypedConversion<Fastcgipp::Sql::Data::Blob>::grabIt(Fastcgipp::Sql::Data::Blob& data);
template void Fastcgipp::Sql::MySQL::TypedConversion<Fastcgipp::Sql::Data::Text>::grabIt(Fastcgipp::Sql::Data::Text& data);
template<typename T> void Fastcgipp::Sql::MySQL::TypedConversion<T>::grabIt(T& data)
{
	if(data.size() != length) data.resize(length);

	if(length)
	{
		MYSQL_BIND bind;
		std::memset(&bind, 0, sizeof(bind));
		bind.buffer=&data[0];
		bind.buffer_length=length;
		bind.length=&length;
		bind.buffer_type=bufferType;
		if(mysql_stmt_fetch_column(statement, &bind, column, 0)!=0) throw Exceptions::MySQL(statement);
	}
}

template void Fastcgipp::Sql::MySQL::TypedConversion<Fastcgipp::Sql::Data::Blob>::convertParam();
template void Fastcgipp::Sql::MySQL::TypedConversion<Fastcgipp::Sql::Data::Text>::convertParam();
template<typename T> void Fastcgipp::Sql::MySQL::TypedConversion<T>::convertParam()
{
	T& data = *(T*)external;

	length = data.size();
	buffer = &data[0];
}

void Fastcgipp::Sql::MySQL::TypedConversion<Fastcgipp::Sql::Data::Wtext>::convertResult()
{
	using namespace std;
	
	vector<char>& conversionBuffer = inputBuffer;
	grabIt(conversionBuffer);

	wstring& output = *(wstring*)external;
	output.resize(conversionBuffer.size());

	if(conversionBuffer.size());
	{
		wchar_t* it;
		const char* tmp;
		mbstate_t conversionState = mbstate_t();
		if(use_facet<codecvt<wchar_t, char, mbstate_t> >(locale(locale::classic(), new utf8CodeCvt::utf8_codecvt_facet)).in(conversionState, (const char*)&conversionBuffer.front(), (const char*)&conversionBuffer.front() + conversionBuffer.size(), tmp, &output[0], &output[0] + output.size(), it)!=codecvt_base::ok) throw Exceptions::CodeCvt();
		output.resize(it-&output[0]);
		conversionBuffer.clear();
	}
}

void Fastcgipp::Sql::MySQL::TypedConversion<Fastcgipp::Sql::Data::Wtext>::convertParam()
{
	using namespace std;

	wstring& data = *(wstring*)external;

	inputBuffer.resize(data.size()*sizeof(wchar_t));

	if(inputBuffer.size())
	{
		const wchar_t* tmp;
		char* it;
		mbstate_t conversionState = mbstate_t();
		if(use_facet<codecvt<wchar_t, char, mbstate_t> >(locale(locale::classic(), new utf8CodeCvt::utf8_codecvt_facet)).out(conversionState, (const wchar_t*)&data[0], (const wchar_t*)&data[0] + data.size(), tmp, &inputBuffer.front(), &inputBuffer.front() + inputBuffer.size(), it)!=codecvt_base::ok) throw Exceptions::CodeCvt();
		inputBuffer.resize(it-&inputBuffer[0]);
	}

	buffer=&inputBuffer.front();
	length = inputBuffer.size();
}

Fastcgipp::Exceptions::MySQL::MySQL(MYSQL* mysql): CodedException(mysql_error(mysql), mysql_errno(mysql)) { }
Fastcgipp::Exceptions::MySQL::MySQL(MYSQL_STMT* stmt): CodedException(mysql_stmt_error(stmt), mysql_stmt_errno(stmt)) { }
