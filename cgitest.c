#include <stdlib.h>
#include <stdio.h>

static char *html = "\
<html>\n\
    <head>\n\
        <meta http-equiv='Content-Type' content='text/html;charset=iso-8859-1'>\n\
        <style type='text/css'>\n\
            input[type='text'  ] { width:220px; font-size:16px; }\n\
            input[type='submit'] { width:100px; font-size:16px; }\n\
            h1 { font-size:32px; }\n\
            td { font-size:18px; }\n\
        </style>\n\
        <title>ffhttpd cgi test</title>\n\
    </head>\n\
    <body>\n\
        <h1>ffhttpd cgi test</h1><hr/>\n\
        <p>hello ffhttpd cgi !</p>\n\
    </body>\n\
</html>\n\
";

int cgimain(char *request_type, char *request_path, char *url_args, char *request_data, int request_size, char *page_buf, int pbuf_size)
{
    return snprintf(page_buf, pbuf_size, html);
}
