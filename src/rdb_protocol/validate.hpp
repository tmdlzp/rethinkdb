#ifndef RDB_PROTOCOL_VALIDATE
#define RDB_PROTOCOL_VALIDATE

#include "rdb_protocol/ql2.pb.h"
#include "rdb_protocol/ql2_extensions.pb.h"

void validate_pb(const Query &q);
void validate_pb(const Query::AssocPair &ap);
void validate_pb(const Frame &f);
void validate_pb(const Backtrace &bt);
void validate_pb(const Response &r);
void validate_pb(const Datum &d);
void validate_pb(const Datum::AssocPair &ap);
void validate_pb(const Term &t);
void validate_pb(const Term::AssocPair &ap);

#endif // RDB_PROTOCOL_VALIDATE