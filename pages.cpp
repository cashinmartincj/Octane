#include "HttpRequest.h"
#include "HttpResponse.h"
#include <string>

template <typename T>
class Obj
{
    std::string parse()
    {
        return static_cast<T*>(this)->serialize();
    }
};

class page : public Obj<page>
{
    private:
        friend class Obj<page>;
        HttpRequest request;
        int status = 200;
        ContentType content_type = ContentType::TEXT_PLAIN;
        std::string serialize()
        {
            std::string response;
            response += "HTTP/1.1 " + std::to_string(status) + "\r\n";
            response += "Content-Type: " + content_type_str[request.content_type] + "\r\n";
            response += "Content-Length: " + std::to_string(request.body.size()) + "\r\n";
            
            for (auto& [key, val] : request.headers) {
                response += key + ": " + val + "\r\n";
            }
            
            response += "\r\n";
            response += std::move(request.body);
            return response;
        }
    public:
        page(HttpRequest&& req) : request(std::move(req))
        {
        }
};