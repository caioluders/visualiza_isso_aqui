#pragma once

#include <string>
#include <chrono>
#include <filesystem>
#include <vector>

#include <glad/glad.h>

class ShaderProgram {
public:
	ShaderProgram();
	~ShaderProgram();

	bool buildFromFiles(const char* fragmentPath);
	void updateIfChanged(const char* fragmentPath);
	void forceReload(const char* fragmentPath);

	void use() const;
	void setUniform(const char* name, float x) const;
	void setUniform(const char* name, float x, float y) const;
    void setUniform(const char* name, float x, float y, float z) const;
    void setUniform(const char* name, float x, float y, float z, float w) const;
    void setUniformi(const char* name, int x) const;
	void setUniform1fv(const char* name, const float* v, int count) const;

    struct UniformInfo {
        std::string name;
        GLenum type;
        GLint size;
        GLint location;
    };
    std::vector<UniformInfo> getActiveUniforms() const;

	// Introspection helper
	GLuint getProgramId() const { return programId; }

private:
	GLuint programId;
	GLuint vertexId;
	GLuint fragmentId;
	std::string currentFragmentPath;
	std::filesystem::file_time_type lastWriteTime;

	bool compile(const char* fragmentPath);
	GLuint compileShader(GLenum type, const char* source);
	std::string readFile(const char* path);
};


