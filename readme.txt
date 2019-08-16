100 多行代码实现了一个简单的 http 服务器

端口 8080
实现了对 GET/POST/HEAD 请求的支持

测试方法：
1. 运行 ffhttpd.exe
2. ffhttpd.exe 所在目录放 html 文件，比如 index.html
3. 打开浏览器输入 http://localhost:8080/index.html

实现的功能：
1. 极小的内存占用，无动态内存分配
2. 基于线程池的多线程模式
3. 支持 GET/POST/HEAD 请求
4. 支持 GET 请求的 range bytes 方式



