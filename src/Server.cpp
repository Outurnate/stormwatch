/* stormwatch
 * Copyright (C) 2020 Joe Dillon
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include "Server.hpp"

#include <restinio/all.hpp>
#include <cmrc/cmrc.hpp>
#include <spdlog/spdlog.h>
#include <nlohmann/json.hpp>

CMRC_DECLARE(web_resources);

using json = nlohmann::json;
using router_t = restinio::router::express_router_t<restinio::router::std_regex_engine_t>;

template <typename RESP>
inline RESP init(RESP resp)
{
	resp.append_header("Server", "stormwatch");
	resp.append_header_date_field();

	return resp;
}

inline std::string ReadFile(const cmrc::file& file)
{
  std::string contents;
  for (const char& i : file)
    contents += i;
  return contents;
}

inline void StaticDirectoryMapper(std::unique_ptr<router_t>& router, std::shared_ptr<cmrc::embedded_filesystem> fs, std::string path, std::string mime)
{
  router->http_get(
    fmt::format("/{}/([a-zA-Z0-9\\.:-]+)", path),
    [fs, path, mime](auto req, auto params)
    {
      auto requestedFile = fmt::format("{}/{}", path, std::string(params[0]));
      if (fs->exists(requestedFile))
      {
        return init(req->create_response())
          .append_header(restinio::http_field::content_type, mime)
          .set_body(ReadFile(fs->open(requestedFile)))
          .done();
      }
      else
        return restinio::request_rejected();
    });
}

inline auto Server::CreateHandler()
{
  auto router = std::make_unique<router_t>();
  std::shared_ptr<cmrc::embedded_filesystem> fs = std::make_shared<cmrc::embedded_filesystem>(cmrc::web_resources::get_filesystem());

  router->http_get(
    "/live.jpeg",
    [this](auto req, auto)
    {
      return init(req->create_response())
        .append_header(restinio::http_field::content_type, "image/jpeg")
        .set_body(camera.GetPreview())
        .done();
    });
  
  router->http_post(
    "/start",
    [this](auto req, auto)
    {
      camera.Start();

      return init(req->create_response())
        .append_header(restinio::http_field::content_type, "text/json; charset=utf-8")
        .set_body("[]")
        .done();
    });
  
  router->http_post(
    "/stop",
    [this](auto req, auto)
    {
      camera.Stop();
      
      return init(req->create_response())
        .append_header(restinio::http_field::content_type, "text/json; charset=utf-8")
        .set_body("[]")
        .done();
    });

  router->http_get(
    "/stats",
    [this](auto req, auto)
    {
      auto cameraStatus = camera.GetStatus();
      json stats;
      stats["width"]       = cameraStatus.resolution.width;
      stats["height"]      = cameraStatus.resolution.height;
      stats["nominalFPS"]  = cameraStatus.nominalFPS;
      stats["measuredFPS"] = cameraStatus.measuredFPS;
      stats["enabled"]     = camera.IsRunning();

      return init(req->create_response())
        .append_header(restinio::http_field::content_type, "text/json; charset=utf-8")
        .set_body(stats.dump())
        .done();
    });

  router->http_get(
    "/clips",
    [this](auto req, auto)
    {
      json clips;
      for (const auto& clip : library.GetClips())
        clips.push_back(json::object(
          {
            { "title", clip.GetTimestamp() },
            { "video", fmt::format("/clips/{}.webm", clip.GetID()) },
            { "thumbnail", fmt::format("/clips/{}.jpeg", clip.GetID()) }
          }));

      return init(req->create_response())
        .append_header(restinio::http_field::content_type, "text/json; charset=utf-8")
        .set_body(clips.dump())
        .done();
    });
  
  router->http_get(
    "/clips/([A-Za-z0-9\\+/=]+)\\.(jpeg|webm)",
    [this](auto req, auto params)
    {
      if (params[1] == "jpeg")
      {
        if (auto clipPath = library.GetClipThumbnailPath(VideoID(params[0])); clipPath)
        {
          return init(req->create_response())
            .append_header(restinio::http_field::content_type, "image/jpeg")
            .set_body(restinio::sendfile(clipPath.value().string()))
            .done();
        }
        return restinio::request_rejected();
      }
      else if (params[1] == "webm")
      {
        if (auto clipPath = library.GetClipVideoPath(VideoID(params[0])); clipPath)
        {
          return init(req->create_response())
            .append_header(restinio::http_field::content_type, "video/webm")
            .set_body(restinio::sendfile(clipPath.value().string()))
            .done();
        }
        return restinio::request_rejected();
      }
      else
        return restinio::request_rejected();
    });

  router->http_delete(
    "/clips/([A-Za-z0-9\\+/=]+)\\.(webm)",
    [this](auto req, auto params)
    {
      auto result = library.DeleteClip(VideoID(params[0]));

      return init(req->create_response())
        .append_header(restinio::http_field::content_type, "text/plain; charset=utf-8")
        .set_body(result ? "true" : "false")
        .done();
    });

  router->http_get(
    "/settings",
    [this](auto req, auto)
    {
      json settings;
      for (auto property : CameraPropertyEntries)
        settings[std::string(property.second)] = camera.GetProperty(property.first);
      return init(req->create_response())
        .append_header(restinio::http_field::content_type, "text/json; charset=utf-8")
        .set_body(settings.dump())
        .done();
    });

  router->http_post(
    "/settings",
    [this](restinio::request_handle_t req, auto)
    {
      const auto parameters = restinio::parse_query(req->body());
      for (auto property : CameraPropertyEntries)
        camera.SetProperty(property.first, restinio::value_or(parameters, property.second, camera.GetProperty(property.first)));
      camera.ApplyPropertyChange();
      return init(req->create_response())
        .append_header(restinio::http_field::content_type, "text/json; charset=utf-8")
        .set_body("{}")
        .done();
    });

  StaticDirectoryMapper(router, fs, "js", "text/javascript; charset=utf-8");
  StaticDirectoryMapper(router, fs, "css", "text/css; charset=utf-8");
  StaticDirectoryMapper(router, fs, "svg", "image/svg+xml");
  StaticDirectoryMapper(router, fs, "png", "image/png");

  router->http_get(
    "/",
    [fs](auto req, auto)
    {
      return init(req->create_response())
        .append_header(restinio::http_field::content_type, "text/html; charset=utf-8")
        .set_body(ReadFile(fs->open("index.html")))
        .done();
    });

  router->non_matched_request_handler(
    [](auto req)
    {
      return init(req->create_response(restinio::status_not_found()))
        .connection_close()
        .done();
    });

  return router;
}

class spdlog_logger_t
{
public:
  template<typename Msg_Builder>
  void trace(Msg_Builder&& mb)
  {
    spdlog::get("web")->trace(mb());
  }

  template<typename Msg_Builder>
  void info(Msg_Builder&& mb)
  {
    spdlog::get("web")->info(mb());
  }

  template<typename Msg_Builder>
  void warn(Msg_Builder&& mb)
  {
    spdlog::get("web")->warn(mb());
  }

  template<typename Msg_Builder>
  void error(Msg_Builder&& mb)
  {
    spdlog::get("web")->error(mb());
  }
};

Server::Server()
 : camera(library)
{
}

void Server::Run(const std::string& address, uint16_t port)
{
  using traits_t =
    restinio::traits_t<
      restinio::asio_timer_manager_t,
      spdlog_logger_t,
      router_t>;

  restinio::run(
    restinio::on_this_thread<traits_t>()
      .port(port)
      .address(address)
      .request_handler(CreateHandler()));
}