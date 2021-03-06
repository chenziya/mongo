/**
*    Copyright (C) 2012 10gen Inc.
*
*    This program is free software: you can redistribute it and/or  modify
*    it under the terms of the GNU Affero General Public License, version 3,
*    as published by the Free Software Foundation.
*
*    This program is distributed in the hope that it will be useful,
*    but WITHOUT ANY WARRANTY; without even the implied warranty of
*    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*    GNU Affero General Public License for more details.
*
*    You should have received a copy of the GNU Affero General Public License
*    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*
*    As a special exception, the copyright holders give permission to link the
*    code of portions of this program with the OpenSSL library under certain
*    conditions as described in each individual source file and distribute
*    linked combinations including the program with the OpenSSL library. You
*    must comply with the GNU Affero General Public License in all respects for
*    all of the code used other than as permitted herein. If you modify file(s)
*    with this exception, you may extend this exception to your version of the
*    file(s), but you are not obligated to do so. If you do not wish to do so,
*    delete this exception statement from your version. If you delete this
*    exception statement from all source files in the program, then also delete
*    it in the license file.
*/

#include "mongo/db/auth/authz_manager_external_state.h"

#include "mongo/base/status.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/security_key.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/namespace_string.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

    AuthzManagerExternalState::AuthzManagerExternalState() {}
    AuthzManagerExternalState::~AuthzManagerExternalState() {}

    Status AuthzManagerExternalState::getPrivilegeDocument(const UserName& userName,
                                                           int authzVersion,
                                                           BSONObj* result) {
        if (userName == internalSecurity.user->getName()) {
            return Status(ErrorCodes::InternalError,
                          "Requested privilege document for the internal user");
        }

        StringData dbname = userName.getDB();

        if (!NamespaceString::validDBName(dbname)) {
            return Status(ErrorCodes::BadValue,
                          mongoutils::str::stream() << "Bad database name \"" << dbname << "\"");
        }

        // Build the query needed to get the privilege document
        std::string usersNamespace;
        BSONObjBuilder queryBuilder;
        if (authzVersion == 1) {
            usersNamespace = mongoutils::str::stream() << dbname << ".system.users";
            queryBuilder.append(AuthorizationManager::V1_USER_NAME_FIELD_NAME, userName.getUser());
            queryBuilder.appendNull(AuthorizationManager::V1_USER_SOURCE_FIELD_NAME);
        } else if (authzVersion == 2) {
            usersNamespace = "admin.system.users";
            queryBuilder.append(AuthorizationManager::USER_NAME_FIELD_NAME, userName.getUser());
            queryBuilder.append(AuthorizationManager::USER_SOURCE_FIELD_NAME, userName.getDB());
        } else {
            return Status(ErrorCodes::UnsupportedFormat,
                          mongoutils::str::stream() <<
                                  "Unrecognized authorization format version: " << authzVersion);
        }

        // Query for the privilege document
        BSONObj userBSONObj;
        Status found = _findUser(usersNamespace, queryBuilder.obj(), &userBSONObj);
        if (!found.isOK()) {
            if (found.code() == ErrorCodes::UserNotFound) {
                // Return more detailed status that includes user name.
                return Status(ErrorCodes::UserNotFound,
                              mongoutils::str::stream() << "auth: couldn't find user " <<
                                      userName.toString() << ", " << usersNamespace,
                              0);
            } else {
                return found;
            }
        }

        *result = userBSONObj.getOwned();
        return Status::OK();
    }

    bool AuthzManagerExternalState::hasAnyPrivilegeDocuments() {
        std::string usersNamespace = "admin.system.users";

        BSONObj userBSONObj;
        BSONObj query;
        return _findUser(usersNamespace, query, &userBSONObj).isOK();
    }


    Status AuthzManagerExternalState::insertPrivilegeDocument(const string& dbname,
                                                              const BSONObj& userObj,
                                                              const BSONObj& writeConcern) {
        Status status = insert(NamespaceString("admin.system.users"), userObj, writeConcern);
        if (status.isOK()) {
            return status;
        }
        if (status.code() == ErrorCodes::DuplicateKey) {
            std::string name = userObj[AuthorizationManager::USER_NAME_FIELD_NAME].String();
            std::string source = userObj[AuthorizationManager::USER_SOURCE_FIELD_NAME].String();
            return Status(ErrorCodes::DuplicateKey,
                          mongoutils::str::stream() << "User \"" << name << "@" << source <<
                                  "\" already exists");
        }
        if (status.code() == ErrorCodes::UnknownError) {
            return Status(ErrorCodes::UserModificationFailed, status.reason());
        }
        return status;
    }

    Status AuthzManagerExternalState::updatePrivilegeDocument(
            const UserName& user, const BSONObj& updateObj, const BSONObj& writeConcern) {
        Status status = updateOne(
                NamespaceString("admin.system.users"),
                BSON(AuthorizationManager::USER_NAME_FIELD_NAME << user.getUser() <<
                     AuthorizationManager::USER_SOURCE_FIELD_NAME << user.getDB()),
                updateObj,
                false,
                writeConcern);
        if (status.isOK()) {
            return status;
        }
        if (status.code() == ErrorCodes::NoMatchingDocument) {
            return Status(ErrorCodes::UserNotFound,
                          mongoutils::str::stream() << "User " << user.getFullName() <<
                                  " not found");
        }
        if (status.code() == ErrorCodes::UnknownError) {
            return Status(ErrorCodes::UserModificationFailed, status.reason());
        }
        return status;
    }

    Status AuthzManagerExternalState::removePrivilegeDocuments(const BSONObj& query,
                                                               const BSONObj& writeConcern,
                                                               int* numRemoved) {
        Status status = remove(NamespaceString("admin.system.users"),
                               query,
                               writeConcern,
                               numRemoved);
        if (status.code() == ErrorCodes::UnknownError) {
            return Status(ErrorCodes::UserModificationFailed, status.reason());
        }
        return status;
    }

    Status AuthzManagerExternalState::updateOne(
            const NamespaceString& collectionName,
            const BSONObj& query,
            const BSONObj& updatePattern,
            bool upsert,
            const BSONObj& writeConcern) {
        int numUpdated;
        Status status = update(collectionName,
                               query,
                               updatePattern,
                               upsert,
                               false,
                               writeConcern,
                               &numUpdated);
        if (!status.isOK()) {
            return status;
        }
        dassert(numUpdated == 1 || numUpdated == 0);
        if (numUpdated == 0) {
            return Status(ErrorCodes::NoMatchingDocument, "No document found");
        }
        return Status::OK();
    }

}  // namespace mongo
