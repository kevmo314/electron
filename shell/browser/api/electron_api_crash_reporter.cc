// Copyright (c) 2013 GitHub, Inc.
// Use of this source code is governed by the MIT license that can be
// found in the LICENSE file.

#include "shell/browser/api/electron_api_crash_reporter.h"

#include <map>
#include <string>

#include "base/bind.h"
#include "base/command_line.h"
#include "base/no_destructor.h"
#include "base/path_service.h"
#include "base/threading/thread_restrictions.h"
#include "chrome/browser/crash_upload_list/crash_upload_list_crashpad.h"
#include "chrome/common/chrome_paths.h"
#include "components/crash/core/app/crashpad.h"
#include "components/crash/core/common/crash_key.h"
#include "components/upload_list/crash_upload_list.h"
#include "components/upload_list/text_log_upload_list.h"
#include "content/public/common/content_switches.h"
#include "gin/arguments.h"
#include "gin/data_object_builder.h"
#include "services/service_manager/embedder/switches.h"
#include "shell/app/electron_crash_reporter_client.h"
#include "shell/common/crash_keys.h"
#include "shell/common/gin_converters/callback_converter.h"
#include "shell/common/gin_converters/file_path_converter.h"
#include "shell/common/gin_converters/time_converter.h"
#include "shell/common/gin_helper/dictionary.h"
#include "shell/common/node_includes.h"

#if defined(OS_POSIX) && !defined(OS_MACOSX)
#include "components/crash/core/app/breakpad_linux.h"
#include "v8/include/v8-wasm-trap-handler-posix.h"
#include "v8/include/v8.h"
#endif

namespace {

std::map<std::string, std::string>& GetGlobalCrashKeysMutable() {
  static base::NoDestructor<std::map<std::string, std::string>>
      global_crash_keys;
  return *global_crash_keys;
}

bool g_crash_reporter_initialized = false;

}  // namespace

namespace electron {

namespace api {

namespace crash_reporter {

bool IsCrashReporterEnabled() {
  return g_crash_reporter_initialized;
}

#if defined(OS_LINUX)
const std::map<std::string, std::string>& GetGlobalCrashKeys() {
  return GetGlobalCrashKeysMutable();
}
#endif

}  // namespace crash_reporter

}  // namespace api

}  // namespace electron

namespace {

typedef std::pair<int, std::string> UploadReportResult;  // upload-date, id

scoped_refptr<UploadList> CreateCrashUploadList() {
#if defined(OS_MACOSX) || defined(OS_WIN)
  return new CrashUploadListCrashpad();
#else

  if (crash_reporter::IsCrashpadEnabled()) {
    return new CrashUploadListCrashpad();
  }

  base::FilePath crash_dir_path;
  base::PathService::Get(chrome::DIR_CRASH_DUMPS, &crash_dir_path);
  base::FilePath upload_log_path =
      crash_dir_path.AppendASCII(CrashUploadList::kReporterLogFilename);
  return new TextLogUploadList(upload_log_path);
#endif  // defined(OS_MACOSX) || defined(OS_WIN)
}

void GetUploadedReports(
    base::OnceCallback<void(v8::Local<v8::Value>)> callback) {
  auto list = CreateCrashUploadList();
  list->Load(base::BindOnce(
      [](scoped_refptr<UploadList> list,
         base::OnceCallback<void(v8::Local<v8::Value>)> callback) {
        std::vector<UploadList::UploadInfo> uploads;
        list->GetUploads(100, &uploads);
        v8::Isolate* isolate = v8::Isolate::GetCurrent();
        v8::HandleScope scope(isolate);
        std::vector<v8::Local<v8::Object>> result;
        for (const auto& upload : uploads) {
          result.push_back(gin::DataObjectBuilder(isolate)
                               .Set("date", upload.upload_time)
                               .Set("id", upload.upload_id)
                               .Build());
        }
        v8::Local<v8::Value> v8_result = gin::ConvertToV8(isolate, result);
        std::move(callback).Run(v8_result);
      },
      list, std::move(callback)));
}

void SetCrashKeysFromMap(const std::map<std::string, std::string>& extra) {
  for (const auto& pair : extra) {
    electron::crash_keys::SetCrashKey(pair.first, pair.second);
  }
}

void SetUploadToServer(bool upload) {
  ElectronCrashReporterClient::Get()->SetCollectStatsConsent(upload);
}

bool GetUploadToServer() {
  return ElectronCrashReporterClient::Get()->GetCollectStatsConsent();
}

void Start(const std::string& submit_url,
           const std::string& crashes_directory,
           bool upload_to_server,
           bool ignore_system_crash_handler,
           bool rate_limit,
           bool compress,
           const std::map<std::string, std::string>& extra_global,
           const std::map<std::string, std::string>& extra) {
  if (g_crash_reporter_initialized)
    return;
  g_crash_reporter_initialized = true;
  SetUploadToServer(upload_to_server);
  auto* command_line = base::CommandLine::ForCurrentProcess();
  std::string process_type =
      command_line->GetSwitchValueASCII(::switches::kProcessType);
  if (crash_reporter::IsCrashpadEnabled()) {
    crash_reporter::InitializeCrashpad(process_type.empty(), process_type);
    // crash_reporter::SetFirstChanceExceptionHandler(v8::TryHandleWebAssemblyTrapPosix);
  } else {
    breakpad::SetUploadURL(submit_url);
    auto& global_crash_keys = GetGlobalCrashKeysMutable();
    for (const auto& pair : extra_global) {
      global_crash_keys[pair.first] = pair.second;
    }
    SetCrashKeysFromMap(extra);
    SetCrashKeysFromMap(extra_global);
    breakpad::InitCrashReporter(process_type);
  }
}

void Initialize(v8::Local<v8::Object> exports,
                v8::Local<v8::Value> unused,
                v8::Local<v8::Context> context,
                void* priv) {
  gin_helper::Dictionary dict(context->GetIsolate(), exports);
  dict.SetMethod("start", base::BindRepeating(&Start));
  dict.SetMethod("addExtraParameter",
                 base::BindRepeating(&electron::crash_keys::SetCrashKey));
  dict.SetMethod("removeExtraParameter",
                 base::BindRepeating(&electron::crash_keys::ClearCrashKey));
  /*
  dict.SetMethod("getParameters",
                 base::BindRepeating(&CrashReporter::GetParameters, reporter));
                 */
  dict.SetMethod("getUploadedReports",
                 base::BindRepeating(&GetUploadedReports));
  dict.SetMethod("setUploadToServer", base::BindRepeating(&SetUploadToServer));
  dict.SetMethod("getUploadToServer", base::BindRepeating(&GetUploadToServer));
}

}  // namespace

NODE_LINKED_MODULE_CONTEXT_AWARE(electron_browser_crash_reporter, Initialize)