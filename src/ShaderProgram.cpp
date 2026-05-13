#include "ShaderProgram.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <vector>

static const char* kVertexSrc = R"GLSL(
#version 150
out vec2 vUV;
void main() {
	vec2 pos = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);
	vUV = pos;
	pos = pos * 2.0 - 1.0;
	gl_Position = vec4(pos, 0.0, 1.0);
}
)GLSL";

static const char* kFallbackFrag = R"GLSL(
#version 150
precision highp float;
uniform vec2 u_resolution;
uniform float u_time;
uniform float u_rms, u_bandLow, u_bandMid, u_bandHigh, u_onset;
out vec4 FragColor;
void main() {
	vec2 uv = gl_FragCoord.xy / u_resolution;
	float a = u_rms * 0.8 + u_onset * 0.2;
	vec3 c = 0.2 + vec3(uv.x + a, uv.y + u_bandMid, 0.5 + 0.3 * sin(u_time + u_bandHigh*5.0));
	FragColor = vec4(c, 1.0);
}
)GLSL";

ShaderProgram::ShaderProgram()
	: programId(0), vertexId(0), fragmentId(0) {}

ShaderProgram::~ShaderProgram() {
	if (programId) glDeleteProgram(programId);
	if (vertexId) glDeleteShader(vertexId);
	if (fragmentId) glDeleteShader(fragmentId);
}

bool ShaderProgram::buildFromFiles(const char* fragmentPath) {
	currentFragmentPath = fragmentPath ? fragmentPath : "";
	return compile(fragmentPath);
}

void ShaderProgram::updateIfChanged(const char* fragmentPath) {
	if (fragmentPath == nullptr || currentFragmentPath.empty()) return;
	std::error_code ec;
	auto t = std::filesystem::last_write_time(currentFragmentPath, ec);
	if (!ec && t != lastWriteTime) {
        compile(currentFragmentPath.c_str());
	}
}

void ShaderProgram::forceReload(const char* fragmentPath) {
	if (fragmentPath == nullptr) fragmentPath = currentFragmentPath.c_str();
	compile(fragmentPath);
}

void ShaderProgram::use() const {
	glUseProgram(programId);
}

void ShaderProgram::setUniform(const char* name, float x) const {
	GLint loc = getUniformLocationCached(name);
	if (loc >= 0) glUniform1f(loc, x);
}

void ShaderProgram::setUniform(const char* name, float x, float y) const {
	GLint loc = getUniformLocationCached(name);
	if (loc >= 0) glUniform2f(loc, x, y);
}

void ShaderProgram::setUniform(const char* name, float x, float y, float z) const {
    GLint loc = getUniformLocationCached(name);
    if (loc >= 0) glUniform3f(loc, x, y, z);
}

void ShaderProgram::setUniform(const char* name, float x, float y, float z, float w) const {
    GLint loc = getUniformLocationCached(name);
    if (loc >= 0) glUniform4f(loc, x, y, z, w);
}

void ShaderProgram::setUniformi(const char* name, int x) const {
    GLint loc = getUniformLocationCached(name);
    if (loc >= 0) glUniform1i(loc, x);
}

void ShaderProgram::setUniform1fv(const char* name, const float* v, int count) const {
	GLint loc = getUniformLocationCached(name);
	if (loc >= 0) glUniform1fv(loc, count, v);
}

bool ShaderProgram::compile(const char* fragmentPath) {
	if (vertexId) { glDeleteShader(vertexId); vertexId = 0; }
	if (fragmentId) { glDeleteShader(fragmentId); fragmentId = 0; }
	if (programId) { glDeleteProgram(programId); programId = 0; }
	uniformLocationCache.clear();
	activeUniformCache.clear();
	activeUniformCacheValid = false;

	vertexId = compileShader(GL_VERTEX_SHADER, kVertexSrc);
    std::string fragSrc = kFallbackFrag;
    if (fragmentPath) {
        // Try provided path first
        if (std::filesystem::exists(fragmentPath)) {
            fragSrc = readFile(fragmentPath);
            std::error_code ec;
            lastWriteTime = std::filesystem::last_write_time(fragmentPath, ec);
        } else {
            // Also try resolving relative to build dir vs source dir
            std::string alt1 = std::string("../") + fragmentPath;
            std::string alt2 = std::string("assets/shaders/") + std::filesystem::path(fragmentPath).filename().string();
            if (std::filesystem::exists(alt1)) {
                fragSrc = readFile(alt1.c_str());
                std::error_code ec;
                lastWriteTime = std::filesystem::last_write_time(alt1, ec);
                currentFragmentPath = alt1;
            } else if (std::filesystem::exists(alt2)) {
                fragSrc = readFile(alt2.c_str());
                std::error_code ec;
                lastWriteTime = std::filesystem::last_write_time(alt2, ec);
                currentFragmentPath = alt2;
            }
        }
    }
	fragmentId = compileShader(GL_FRAGMENT_SHADER, fragSrc.c_str());
	if (!vertexId || !fragmentId) return false;

	programId = glCreateProgram();
	glAttachShader(programId, vertexId);
	glAttachShader(programId, fragmentId);
	glLinkProgram(programId);

	GLint linked = 0;
	glGetProgramiv(programId, GL_LINK_STATUS, &linked);
	if (!linked) {
		char log[2048];
		GLsizei len = 0;
		glGetProgramInfoLog(programId, sizeof(log), &len, log);
		std::fprintf(stderr, "Program link error: %s\n", log);
		glDeleteProgram(programId);
		programId = 0;
		return false;
	}
	return true;
}

GLint ShaderProgram::getUniformLocationCached(const char* name) const {
	if (!name || !programId) return -1;
	auto it = uniformLocationCache.find(name);
	if (it != uniformLocationCache.end()) return it->second;
	GLint loc = glGetUniformLocation(programId, name);
	uniformLocationCache.emplace(name, loc);
	return loc;
}

GLuint ShaderProgram::compileShader(GLenum type, const char* source) {
	GLuint id = glCreateShader(type);
	glShaderSource(id, 1, &source, nullptr);
	glCompileShader(id);
	GLint compiled = 0;
	glGetShaderiv(id, GL_COMPILE_STATUS, &compiled);
	if (!compiled) {
		char log[2048];
		GLsizei len = 0;
		glGetShaderInfoLog(id, sizeof(log), &len, log);
		std::fprintf(stderr, "%s compile error: %s\n", type == GL_VERTEX_SHADER ? "Vertex" : "Fragment", log);
		glDeleteShader(id);
		return 0;
	}
	return id;
}

std::string ShaderProgram::readFile(const char* path) {
	FILE* f = std::fopen(path, "rb");
	if (!f) return {};
	std::fseek(f, 0, SEEK_END);
	long sz = std::ftell(f);
	std::fseek(f, 0, SEEK_SET);
	std::string s;
	s.resize(sz);
	if (sz > 0) std::fread(&s[0], 1, sz, f);
	std::fclose(f);
	return s;
}

std::vector<ShaderProgram::UniformInfo> ShaderProgram::getActiveUniforms() const {
    if (!programId) return {};
    if (activeUniformCacheValid) return activeUniformCache;

    std::vector<UniformInfo> out;
    GLint count = 0;
    glGetProgramiv(programId, GL_ACTIVE_UNIFORMS, &count);
    GLint maxNameLen = 0;
    glGetProgramiv(programId, GL_ACTIVE_UNIFORM_MAX_LENGTH, &maxNameLen);
    std::vector<char> nameBuf;
    nameBuf.resize(std::max(32, (int)maxNameLen));
    for (GLint i = 0; i < count; ++i) {
        GLsizei len = 0; GLint size = 0; GLenum type = 0;
        glGetActiveUniform(programId, (GLuint)i, (GLsizei)nameBuf.size(), &len, &size, &type, nameBuf.data());
        std::string name(nameBuf.data(), len);
        // Strip array suffix [0]
        if (!name.empty()) {
            size_t p = name.find("[0]");
            if (p != std::string::npos) name = name.substr(0, p);
        }
        GLint loc = glGetUniformLocation(programId, name.c_str());
        out.push_back({name, type, size, loc});
    }
    activeUniformCache = out;
    activeUniformCacheValid = true;
    return activeUniformCache;
}
