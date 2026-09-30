#pragma once

// The FIX edge: QuickFIX messages in, the gateway's own types out, and back.
// Nothing past this file knows what a tag number is.

#include <string>

#include "Types.h"
#include "quickfix/Message.h"
#include "quickfix/fix44/ExecutionReport.h"
#include "quickfix/fix44/NewOrderSingle.h"
#include "quickfix/fix44/OrderCancelReject.h"
#include "quickfix/fix44/OrderCancelReplaceRequest.h"
#include "quickfix/fix44/OrderCancelRequest.h"

namespace fixconv {

NewOrder toNewOrder(const FIX44::NewOrderSingle& m, const std::string& client);
CancelRequest toCancel(const FIX44::OrderCancelRequest& m, const std::string& client);
ReplaceRequest toReplace(const FIX44::OrderCancelReplaceRequest& m, const std::string& client);

FIX44::ExecutionReport toFix(const ExecReport& r);
FIX44::OrderCancelReject toFix(const CancelReject& r);

// true if the message came in as a resend (PossDupFlag 43=Y or PossResend 97=Y)
bool isResend(const FIX::Message& m);

}  // namespace fixconv
