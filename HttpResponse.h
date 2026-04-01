#pragma once
#include <string>
#include <unordered_map>

enum class ContentType {
    TEXT_PLAIN,
    TEXT_HTML,
    APPLICATION_JSON,
    APPLICATION_XML,
    IMAGE_JPEG,
    IMAGE_PNG,
    APPLICATION_PDF,
    OCTET_STREAM
};

static std::unordered_map<ContentType, std::string> content_type_str
{
    { ContentType::TEXT_PLAIN, "text/plain" },
    { ContentType::TEXT_HTML, "text/html" },
    { ContentType::APPLICATION_JSON, "application/json" },
    { ContentType::APPLICATION_XML, "application/xml" },
    { ContentType::IMAGE_JPEG, "image/jpeg" },
    { ContentType::IMAGE_PNG, "image/png" },
    { ContentType::APPLICATION_PDF, "application/pdf" },
    { ContentType::OCTET_STREAM, "application/octet-stream" }
};


struct HttpResponse {
    int status = 200;
    ContentType content_type = ContentType::TEXT_PLAIN;
    std::string body;
    std::unordered_map<std::string, std::string> headers;

    // Helper methods
    void text(const std::string& content);
    void json(const std::string& content);
    void html(const std::string& content);
    void image(const std::string& data, ContentType type);
    void send(const std::string& content);

    // Serialize to raw HTTP response string
    std::string serialize() const;
};