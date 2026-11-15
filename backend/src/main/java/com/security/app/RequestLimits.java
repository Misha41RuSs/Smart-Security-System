package com.security.app;

import jakarta.servlet.*;
import jakarta.servlet.http.*;
import org.springframework.stereotype.Component;
import org.springframework.web.filter.OncePerRequestFilter;
import java.io.IOException;
import java.util.concurrent.ConcurrentHashMap;

@Component
public class RequestLimits extends OncePerRequestFilter {
    private record Window(long start,int count) {}
    private final ConcurrentHashMap<String,Window> logins = new ConcurrentHashMap<>();
    @Override protected void doFilterInternal(HttpServletRequest req,HttpServletResponse res,FilterChain chain) throws ServletException,IOException {
        res.setHeader("Cache-Control","no-store");
        if ("POST".equals(req.getMethod()) && (req.getContentLengthLong()>8192 || req.getContentLengthLong()<0)) {
            res.sendError(413); return;
        }
        if (req.getRequestURI().startsWith("/api/auth/")) {
            long now=System.currentTimeMillis();
            logins.entrySet().removeIf(e -> now-e.getValue().start()>=60000);
            if (logins.size()>=4096) { res.sendError(429); return; }
            Window w=logins.compute(req.getRemoteAddr(),(k,v)->v==null ? new Window(now,1):new Window(v.start(),v.count()+1));
            if(w.count()>20) { res.sendError(429); return; }
        }
        chain.doFilter(req,res);
    }
}
